// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "folderbackup.h"
#include "dbusnames.h"
#include "graphclient.h"

#include <KLocalizedString>

#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUrl>

namespace GraphSync::FolderBackup
{

namespace
{
bool inside(const QString &path, const QString &folder)
{
    return path == folder || path.startsWith(folder + QLatin1Char('/'));
}

// The path with symbolic links resolved where it exists, else just cleaned
QString resolved(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
}
}

QList<Folder> folders()
{
    return {
        {QStringLiteral("desktop"), QStringLiteral("Desktop"), i18n("Desktop"), QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)},
        {QStringLiteral("documents"), QStringLiteral("Documents"), i18n("Documents"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)},
        {QStringLiteral("pictures"), QStringLiteral("Pictures"), i18n("Pictures"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)},
    };
}

std::optional<Folder> folder(const QString &key)
{
    const QList<Folder> all = folders();
    for (const Folder &each : all) {
        if (each.key == key) {
            return each;
        }
    }
    return std::nullopt;
}

QString problem(const QString &localPath, const QList<RootConfig> &roots)
{
    if (localPath.isEmpty() || !QDir::isAbsolutePath(localPath)) {
        return i18n("This computer has no such folder.");
    }
    const QFileInfo info(localPath);
    if (info.exists() && !info.isDir()) {
        return i18n("%1 is not a folder.", localPath);
    }
    const QString clean = QDir::cleanPath(localPath);
    const QString real = resolved(localPath);
    const QString home = resolved(QDir::homePath());
    // XDG may point a user folder at the home folder itself (XDG_DESKTOP_DIR="$HOME")
    if (real == QLatin1String("/") || real == home || inside(home, real) || clean == QDir::cleanPath(QDir::homePath())) {
        return i18n("%1 is the home folder; only a folder of its own can be backed up.", localPath);
    }
    for (const RootConfig &root : roots) {
        const QString other = QDir::cleanPath(root.localPath);
        const QString otherReal = resolved(root.localPath);
        if (inside(clean, other) || inside(other, clean) || inside(real, otherReal) || inside(otherReal, real)) {
            return clean == other || real == otherReal ? i18n("%1 is synced already.", localPath)
                                                       : i18n("%1 is inside or around the synced folder %2.", localPath, root.localPath);
        }
    }
    return {};
}

RootConfig prepare(GraphClient *graph, const QString &accountId, const Folder &folder, QString *onlineName, QString *error)
{
    RootConfig root;
    // The account's own OneDrive
    const GraphResponse drive = graph->get(QStringLiteral("/me/drive?$select=id,webUrl"));
    if (!drive.ok()) {
        *error = i18n("Cannot reach the OneDrive of this account: %1", drive.describe());
        return {};
    }
    const QString driveId = drive.json().value(QLatin1String("id")).toString();
    if (driveId.isEmpty()) {
        *error = i18n("This account has no OneDrive.");
        return {};
    }
    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(folder.onlineName));
    GraphResponse item = graph->get(QStringLiteral("/drives/%1/root:/%2?$select=id,name,folder,file,webUrl").arg(driveId, encoded));
    if (item.status == 404) {
        // Not there yet: made now; "fail" never overwrites or renames something that appeared meanwhile
        const QJsonObject body{{QStringLiteral("name"), folder.onlineName},
                               {QStringLiteral("folder"), QJsonObject()},
                               {QStringLiteral("@microsoft.graph.conflictBehavior"), QStringLiteral("fail")}};
        item = graph->postJson(QStringLiteral("/drives/%1/root/children").arg(driveId), body);
        if (item.status == 409) {
            item = graph->get(QStringLiteral("/drives/%1/root:/%2?$select=id,name,folder,file,webUrl").arg(driveId, encoded));
        }
    }
    if (!item.ok()) {
        *error = i18n("Cannot find or create the folder %1 in OneDrive: %2", folder.onlineName, item.describe());
        return {};
    }
    const QJsonObject json = item.json();
    if (!json.contains(QLatin1String("folder"))) {
        *error = i18n("%1 in OneDrive is a file, not a folder.", folder.onlineName);
        return {};
    }
    root.name = QFileInfo(folder.localPath).fileName().isEmpty() ? folder.title : QFileInfo(folder.localPath).fileName();
    root.localPath = QDir::cleanPath(folder.localPath);
    root.driveId = driveId;
    root.remoteRootId = json.value(QLatin1String("id")).toString();
    root.webUrl = json.value(QLatin1String("webUrl")).toString();
    *onlineName = json.value(QLatin1String("name")).toString(folder.onlineName);
    root.kind = QStringLiteral("folder");
    root.accountId = accountId;
    root.backup = folder.key;
    return root;
}

QString start(GraphClient *graph, const QString &accountId, const Folder &folder, QString *error)
{
    QDBusInterface daemon(DBusNames::service(), DBusNames::path(), DBusNames::interfaceName());
    if (!daemon.isValid()) {
        *error = i18n("The sync service is not running.");
        return {};
    }
    if (const QString why = problem(folder.localPath, Config::roots()); !why.isEmpty()) {
        *error = why;
        return {};
    }
    // The OneDrive location will leave the folder out: changes it has not uploaded yet would stay
    // behind in its copy, so it must be done with its work (not paused, offline, busy or signed
    // out). "error" is fine: a file that keeps failing elsewhere must not block this for good.
    if (const auto personal = personalRoot(accountId, Config::roots())) {
        const QDBusReply<QString> status = daemon.call(QStringLiteral("Status"));
        const QJsonArray roots = QJsonDocument::fromJson(status.value().toUtf8()).object().value(QLatin1String("roots")).toArray();
        const auto state = std::find_if(roots.begin(), roots.end(), [&](const QJsonValue &each) {
            return each.toObject().value(QLatin1String("id")).toString() == personal->id;
        });
        // Not known (no answer, location not running): no backup either, rather than risk it
        const QString current = state != roots.end() ? state->toObject().value(QLatin1String("state")).toString() : QString();
        if (current != QLatin1String("idle") && current != QLatin1String("error")) {
            *error = i18n("Wait until %1 is up to date, then start the backup again: it will leave this folder out, and changes it has not uploaded yet would stay behind.", personal->name);
            return {};
        }
    }
    QString onlineName;
    const RootConfig root = prepare(graph, accountId, folder, &onlineName, error);
    if (root.driveId.isEmpty()) {
        return {};
    }
    QDir().mkpath(root.localPath);
    // The OneDrive location leaves that folder out first, so nothing is synced twice
    const auto personal = personalRoot(accountId, Config::roots());
    bool excludedNow = false;
    if (personal) {
        const bool excluded = std::any_of(personal->excludedPaths.cbegin(), personal->excludedPaths.cend(), [&](const QString &path) {
            return path.compare(onlineName, Qt::CaseInsensitive) == 0;
        });
        if (!excluded) {
            daemon.call(QStringLiteral("SetExcludedFolders"), personal->id, personal->excludedPaths + QStringList{onlineName});
            excludedNow = true;
        }
    }
    const QJsonObject request{{QStringLiteral("name"), root.name},
                              {QStringLiteral("localPath"), root.localPath},
                              {QStringLiteral("driveId"), root.driveId},
                              {QStringLiteral("remoteRootId"), root.remoteRootId},
                              {QStringLiteral("kind"), root.kind},
                              {QStringLiteral("webUrl"), root.webUrl},
                              {QStringLiteral("accountId"), accountId},
                              {QStringLiteral("backup"), root.backup}};
    const QDBusReply<QString> added = daemon.call(QStringLiteral("AddRoot"), QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
    if (!added.isValid() || added.value().isEmpty()) {
        // Undone, so the folder is not left synced nowhere
        if (excludedNow) {
            daemon.call(QStringLiteral("SetExcludedFolders"), personal->id, personal->excludedPaths);
        }
        *error = i18n("The sync service refused %1.", root.localPath);
        return {};
    }
    return added.value();
}

std::optional<RootConfig> personalRoot(const QString &accountId, const QList<RootConfig> &roots)
{
    for (const RootConfig &root : roots) {
        if (root.kind == QLatin1String("personal") && root.accountId == accountId) {
            return root;
        }
    }
    return std::nullopt;
}

}
