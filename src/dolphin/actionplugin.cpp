// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "actionplugin.h"

#include <KConfig>
#include <KConfigGroup>
#include <KFileItem>
#include <KFileItemListProperties>
#include <KLocalizedString>
#include <KPluginFactory>

#include <QAction>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QIcon>
#include <QMenu>
#include <QProcess>

K_PLUGIN_CLASS_WITH_JSON(ActionPlugin, "actionplugin.json")

namespace
{
struct Root {
    QString id;
    QString name;
    QString localPath;
    QString kind;
    bool filesOnDemand = false;
};

QList<Root> readRoots()
{
    // Fresh each time: roots are added and removed while Dolphin keeps running
    const KConfig config(QStringLiteral("graphsyncrc"));
    QList<Root> roots;
    const QStringList groups = config.groupList();
    for (const QString &name : groups) {
        if (!name.startsWith(QLatin1String("Root "))) {
            continue;
        }
        const KConfigGroup group = config.group(name);
        if (!group.readEntry("Enabled", true)) {
            continue;
        }
        Root root{name.mid(5),
                  group.readEntry("Name", name.mid(5)),
                  QDir::cleanPath(group.readEntry("LocalPath", QString())),
                  group.readEntry("Kind", QString()),
                  group.readEntry("FilesOnDemand", false)};
        if (!root.localPath.isEmpty()) {
            roots.append(root);
        }
    }
    return roots;
}

// The root that holds path, or nullptr
const Root *rootFor(const QList<Root> &roots, const QString &path)
{
    const Root *owner = nullptr;
    for (const Root &root : roots) {
        if ((path == root.localPath || path.startsWith(root.localPath + QLatin1Char('/')))
            && (!owner || root.localPath.size() > owner->localPath.size())) {
            owner = &root;
        }
    }
    return owner;
}

// Fire and forget: graphsyncd's Command (D-Bus activation starts it when needed)
void sendCommand(const QJsonObject &command)
{
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("io.github.nvaert1986.GraphSync"),
                                                       QStringLiteral("/GraphSync"),
                                                       QStringLiteral("io.github.nvaert1986.GraphSync"),
                                                       QStringLiteral("Command"));
    call << QString::fromUtf8(QJsonDocument(command).toJson(QJsonDocument::Compact));
    QDBusConnection::sessionBus().send(call);
}

void launchApp(const QStringList &arguments)
{
    QProcess::startDetached(QStringLiteral(GRAPHSYNC_BINDIR "/plasma-graphsync-linux"), arguments);
}
}

ActionPlugin::ActionPlugin(QObject *parent, const QVariantList &args)
    : KAbstractFileItemActionPlugin(parent)
{
    Q_UNUSED(args)
}

QList<QAction *> ActionPlugin::actions(const KFileItemListProperties &fileItemInfos, QWidget *parentWidget)
{
    const KFileItemList items = fileItemInfos.items();
    if (items.isEmpty() || !fileItemInfos.isLocal()) {
        return {};
    }

    // Only when everything selected sits in the same sync root
    const QList<Root> roots = readRoots();
    const Root *root = nullptr;
    for (const KFileItem &item : items) {
        const Root *owner = rootFor(roots, QDir::cleanPath(item.localPath()));
        if (!owner || (root && owner->id != root->id)) {
            return {};
        }
        root = owner;
    }
    const Root current = *root;
    const KFileItem first = items.first();
    const QString path = QDir::cleanPath(first.localPath());
    const bool single = items.size() == 1;
    const bool isRoot = single && path == current.localPath;

    auto *menu = new QMenu(parentWidget);

    if (single) {
        // Same order as OneDrive's menu in Windows Explorer
        QAction *share = menu->addAction(QIcon::fromTheme(QStringLiteral("emblem-shared-symbolic")), i18nc("@action:inmenu", "Share…"));
        connect(share, &QAction::triggered, this, [path] {
            launchApp({QStringLiteral("--share"), path});
        });
        QAction *copyLink = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18nc("@action:inmenu", "Copy Link"));
        connect(copyLink, &QAction::triggered, this, [path] {
            launchApp({QStringLiteral("--copy-link"), path});
        });

        // The app asks Graph for the item's own address (Office files open in the browser editor)
        QAction *viewOnline = menu->addAction(QIcon::fromTheme(QStringLiteral("internet-services")), i18nc("@action:inmenu", "View Online"));
        connect(viewOnline, &QAction::triggered, this, [path] {
            launchApp({QStringLiteral("--view-online"), path});
        });

        if (!first.isDir()) {
            QAction *versions = menu->addAction(QIcon::fromTheme(QStringLiteral("view-history")), i18nc("@action:inmenu", "Version History…"));
            connect(versions, &QAction::triggered, this, [path] {
                launchApp({QStringLiteral("--versions"), path});
            });
        }
        menu->addSeparator();
    }

    // Files On-Demand, like OneDrive's Explorer menu; works on several items at once
    if (current.filesOnDemand) {
        QStringList paths;
        QStringList placeholders;
        for (const KFileItem &item : items) {
            const QString local = QDir::cleanPath(item.localPath());
            paths.append(local);
            if (!item.isDir() && local.endsWith(QLatin1String(".cloud"))) {
                placeholders.append(local);
            }
        }
        if (!placeholders.isEmpty()) {
            QAction *download = menu->addAction(QIcon::fromTheme(QStringLiteral("cloud-download")), i18nc("@action:inmenu", "Download Now"));
            connect(download, &QAction::triggered, this, [placeholders] {
                for (const QString &path : placeholders) {
                    sendCommand({{QStringLiteral("command"), QStringLiteral("hydrate")}, {QStringLiteral("path"), path}});
                }
            });
        }
        QAction *keep = menu->addAction(QIcon::fromTheme(QStringLiteral("window-pin")), i18nc("@action:inmenu", "Always Keep on This Device"));
        connect(keep, &QAction::triggered, this, [paths] {
            sendCommand({{QStringLiteral("command"), QStringLiteral("pin")}, {QStringLiteral("paths"), QJsonArray::fromStringList(paths)}, {QStringLiteral("state"), QStringLiteral("always")}});
        });
        QAction *freeUp = menu->addAction(QIcon::fromTheme(QStringLiteral("cloud-upload")), i18nc("@action:inmenu", "Free Up Space"));
        connect(freeUp, &QAction::triggered, this, [paths] {
            sendCommand({{QStringLiteral("command"), QStringLiteral("pin")}, {QStringLiteral("paths"), QJsonArray::fromStringList(paths)}, {QStringLiteral("state"), QStringLiteral("online")}});
        });
        menu->addSeparator();
    }

    QAction *syncNow = menu->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18nc("@action:inmenu", "Sync Now"));
    connect(syncNow, &QAction::triggered, this, [] {
        const QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("io.github.nvaert1986.GraphSync"),
                                                                 QStringLiteral("/GraphSync"),
                                                                 QStringLiteral("io.github.nvaert1986.GraphSync"),
                                                                 QStringLiteral("SyncNow"));
        QDBusConnection::sessionBus().send(call);
    });

    menu->addSeparator();
    // Shortcuts are managed online ("Add shortcut to My files")
    if (isRoot && current.kind != QLatin1String("shortcut")) {
        QAction *chooseFolders = menu->addAction(QIcon::fromTheme(QStringLiteral("folder-sync")), i18nc("@action:inmenu", "Choose Folders…"));
        connect(chooseFolders, &QAction::triggered, this, [id = current.id] {
            launchApp({QStringLiteral("--choose-folders"), id});
        });
    }
    QAction *settings = menu->addAction(QIcon::fromTheme(QStringLiteral("configure")), i18nc("@action:inmenu", "Settings…"));
    connect(settings, &QAction::triggered, this, [] {
        launchApp({});
    });

    // One submenu named after the library, like the folder itself
    auto *submenu = new QAction(QIcon::fromTheme(QStringLiteral("io.github.nvaert1986.graphsync")), current.name, parentWidget);
    submenu->setMenu(menu);
    return {submenu};
}

#include "actionplugin.moc"
