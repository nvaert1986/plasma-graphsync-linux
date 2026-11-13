// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dbusservice.h"
#include "daemon.h"
#include "graphsync_debug.h"
#include "rootcontroller.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>

namespace GraphSync
{

namespace
{
QString toJson(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

RootConfig rootFromJson(const QJsonObject &json)
{
    RootConfig root;
    root.name = json.value(QLatin1String("name")).toString();
    root.localPath = json.value(QLatin1String("localPath")).toString();
    root.driveId = json.value(QLatin1String("driveId")).toString();
    root.remoteRootId = json.value(QLatin1String("remoteRootId")).toString();
    root.kind = json.value(QLatin1String("kind")).toString(QStringLiteral("library"));
    root.webUrl = json.value(QLatin1String("webUrl")).toString();
    // Empty: the current account
    root.accountId = json.value(QLatin1String("accountId")).toString();
    // PC folder backup; only the three folders it knows
    const QString backup = json.value(QLatin1String("backup")).toString();
    if (backup == QLatin1String("desktop") || backup == QLatin1String("documents") || backup == QLatin1String("pictures")) {
        root.backup = backup;
    }
    const QJsonArray excluded = json.value(QLatin1String("excludedPaths")).toArray();
    for (const QJsonValue &path : excluded) {
        root.excludedPaths.append(path.toString());
    }
    return root;
}
}

DBusService::DBusService(Daemon *daemon, QObject *parent)
    : QObject(parent)
    , m_daemon(daemon)
{
    // Status changes come in bursts during a cycle; coalesce them for listeners
    auto *throttle = new QTimer(this);
    throttle->setSingleShot(true);
    throttle->setInterval(250);
    connect(throttle, &QTimer::timeout, this, [this] {
        Q_EMIT StatusChanged(Status());
    });
    connect(daemon, &Daemon::statusChanged, throttle, qOverload<>(&QTimer::start));
    connect(daemon, &Daemon::activityAdded, this, [this](const QVariantMap &entry) {
        Q_EMIT ActivityAdded(toJson(QJsonObject::fromVariantMap(entry)));
    });
}

QString DBusService::Status() const
{
    return toJson(m_daemon->status());
}

QString DBusService::Activity() const
{
    return QString::fromUtf8(QJsonDocument(m_daemon->activity()).toJson(QJsonDocument::Compact));
}

void DBusService::SyncNow()
{
    m_daemon->syncNow();
}

void DBusService::Pause(int minutes)
{
    m_daemon->pause(minutes);
}

void DBusService::Resume()
{
    m_daemon->resume();
}

void DBusService::ReloadAccount()
{
    m_daemon->reloadAccount();
}

void DBusService::SignOut()
{
    m_daemon->signOut();
}

QString DBusService::AddRoot(const QString &json)
{
    const RootConfig root = rootFromJson(QJsonDocument::fromJson(json.toUtf8()).object());
    if (root.localPath.isEmpty() || root.driveId.isEmpty()) {
        return {};
    }
    return m_daemon->addRoot(root);
}

bool DBusService::RemoveRoot(const QString &id)
{
    return m_daemon->removeRoot(id);
}

void DBusService::SetExcludedFolders(const QString &id, const QStringList &paths)
{
    m_daemon->setExcludedFolders(id, paths);
}

void DBusService::ResolveBigDelete(const QString &id, const QString &action)
{
    m_daemon->resolveBigDelete(id, action);
}

void DBusService::Quit()
{
    // Reply first, then leave the event loop
    QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
}

QByteArray DBusService::Thumbnail(const QString &path, int size, const QDBusMessage &message)
{
    message.setDelayedReply(true);
    if (!m_daemon->thumbnail(path, size, message)) {
        QDBusConnection::sessionBus().send(message.createReply(QVariant::fromValue(QByteArray())));
    }
    return {};
}

QString DBusService::Command(const QString &json)
{
    const QJsonObject request = QJsonDocument::fromJson(json.toUtf8()).object();
    const QString command = request.value(QLatin1String("command")).toString();
    const QString id = request.value(QLatin1String("id")).toString();
    if (command == QLatin1String("status")) {
        return Status();
    }
    if (command == QLatin1String("activity")) {
        return Activity();
    }
    if (command == QLatin1String("syncNow")) {
        SyncNow();
    } else if (command == QLatin1String("pause")) {
        Pause(request.value(QLatin1String("minutes")).toInt());
    } else if (command == QLatin1String("resume")) {
        Resume();
    } else if (command == QLatin1String("resolveBigDelete")) {
        ResolveBigDelete(id, request.value(QLatin1String("action")).toString());
    } else if (command == QLatin1String("removeRoot")) {
        return toJson({{QStringLiteral("ok"), RemoveRoot(id)}});
    } else if (command == QLatin1String("setCurrentAccount")) {
        return toJson({{QStringLiteral("ok"), m_daemon->setCurrentAccount(id)}});
    } else if (command == QLatin1String("signOutAccount")) {
        m_daemon->signOut(id);
    } else if (command == QLatin1String("removeAccount")) {
        m_daemon->removeAccount(id);
    } else if (command == QLatin1String("reloadAccount")) {
        // One account signed in (the app, graphsyncctl): only its locations restart
        m_daemon->reloadOneAccount(id);
    } else if (command == QLatin1String("reloadSettings")) {
        m_daemon->reloadSettings();
    } else if (command == QLatin1String("hydrate")) {
        // The .cloud open handler: download this online-only file now
        return toJson({{QStringLiteral("ok"), m_daemon->hydrate(request.value(QLatin1String("path")).toString())}});
    } else if (command == QLatin1String("hydrateState")) {
        return toJson({{QStringLiteral("ok"), true}, {QStringLiteral("state"), m_daemon->hydrationState(request.value(QLatin1String("path")).toString())}});
    } else if (command == QLatin1String("pin")) {
        // Dolphin: Always Keep on This Device ("always") or Free Up Space ("online")
        QStringList paths;
        const QJsonArray list = request.value(QLatin1String("paths")).toArray();
        for (const QJsonValue &value : list) {
            paths.append(value.toString());
        }
        return toJson({{QStringLiteral("ok"), m_daemon->pin(paths, request.value(QLatin1String("state")).toString())}});
    } else if (command == QLatin1String("sharingChanged")) {
        m_daemon->sharingChanged(request.value(QLatin1String("path")).toString());
    } else if (command == QLatin1String("recheck")) {
        // Settings > Advanced > Check All Files Again
        for (RootController *controller : m_daemon->controllers()) {
            controller->recheck();
        }
    } else if (command == QLatin1String("quit")) {
        Quit();
    } else if (command == QLatin1String("launch")) {
        // The tray applet starts the app (setup, sign-in, choose folders) through us. Only the app's
        // normal options pass: not the testing ones (--screenshot writes a file anywhere), not
        // arbitrary arguments from whoever calls the bus
        static const QStringList flags{QStringLiteral("--setup"), QStringLiteral("--add"), QStringLiteral("--sign-in"), QStringLiteral("--add-personal"),
                                       QStringLiteral("--add-account")};
        static const QStringList withValue{QStringLiteral("--choose-folders"), QStringLiteral("--copy-link"), QStringLiteral("--view-online"),
                                           QStringLiteral("--share"), QStringLiteral("--versions")};
        QStringList arguments;
        const QJsonArray list = request.value(QLatin1String("arguments")).toArray();
        for (qsizetype i = 0; i < list.size(); ++i) {
            const QString argument = list.at(i).toString();
            if (flags.contains(argument)) {
                arguments.append(argument);
            } else if (withValue.contains(argument) && i + 1 < list.size() && !list.at(i + 1).toString().startsWith(QLatin1Char('-'))) {
                arguments << argument << list.at(++i).toString();
            } else {
                qCWarning(GRAPHSYNC).noquote() << "Refused to launch the app with" << argument;
                return toJson({{QStringLiteral("ok"), false}});
            }
        }
        QProcess::startDetached(QStringLiteral(GRAPHSYNC_BINDIR "/plasma-graphsync-linux"), arguments);
    } else if (command == QLatin1String("copyText")) {
        const bool copied = m_daemon->copyToClipboard(request.value(QLatin1String("text")).toString());
        return toJson({{QStringLiteral("ok"), copied}});
    } else if (command == QLatin1String("linkCopied")) {
        // From the app's --copy-link run: hold the link and say what it allows
        const bool copied = m_daemon->copyToClipboard(request.value(QLatin1String("url")).toString());
        // The link was made either way
        m_daemon->sharingChanged(request.value(QLatin1String("path")).toString());
        if (copied) {
            m_daemon->notifyLinkCopied(request.value(QLatin1String("path")).toString(),
                                       request.value(QLatin1String("name")).toString(),
                                       request.value(QLatin1String("scope")).toString(),
                                       request.value(QLatin1String("type")).toString());
        }
        return toJson({{QStringLiteral("ok"), copied}});
    } else if (command == QLatin1String("linkFailed")) {
        m_daemon->notifyLinkFailed(request.value(QLatin1String("name")).toString(), request.value(QLatin1String("error")).toString());
    } else if (command == QLatin1String("showInFolder")) {
        const QString path = request.value(QLatin1String("path")).toString();
        QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                              QStringLiteral("/org/freedesktop/FileManager1"),
                                                              QStringLiteral("org.freedesktop.FileManager1"),
                                                              QStringLiteral("ShowItems"));
        message << QStringList{QUrl::fromLocalFile(path).toString()} << QString();
        QDBusConnection::sessionBus().asyncCall(message);
    } else {
        return toJson({{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("unknown command")}});
    }
    return toJson({{QStringLiteral("ok"), true}});
}

}
