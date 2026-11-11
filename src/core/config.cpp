// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"

#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>

#include <QDir>
#include <QFile>

#include <algorithm>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace GraphSync::Config
{

namespace
{
const QString rootPrefix = QStringLiteral("Root ");

KSharedConfigPtr config()
{
    // Full config cascades to /etc/xdg/graphsyncrc, so a package or an admin can ship the app
    // registration ([Auth] ClientId/TenantId) for everyone; user settings still win
    auto cfg = KSharedConfig::openConfig(QStringLiteral("graphsyncrc"));
    cfg->reparseConfiguration();
    return cfg;
}
}

AuthConfig auth()
{
    const KConfigGroup group = config()->group(QStringLiteral("Auth"));
    return {group.readEntry("ClientId", QString()), group.readEntry("TenantId", QString())};
}

void setAuth(const AuthConfig &given)
{
    const AuthConfig auth{given.clientId.trimmed().toLower(), given.tenantId.trimmed().toLower()};
    auto cfg = config();
    KConfigGroup group = cfg->group(QStringLiteral("Auth"));
    group.writeEntry("ClientId", auth.clientId);
    group.writeEntry("TenantId", auth.tenantId);
    cfg->sync();
}

AccountInfo account()
{
    const KConfigGroup group = config()->group(QStringLiteral("Account"));
    return {group.readEntry("ObjectId", QString()),
            group.readEntry("UserPrincipalName", QString()),
            group.readEntry("DisplayName", QString()),
            group.readEntry("TenantId", QString()),
            group.readEntry("Organization", QString())};
}

void setAccount(const AccountInfo &account)
{
    auto cfg = config();
    KConfigGroup group = cfg->group(QStringLiteral("Account"));
    group.writeEntry("ObjectId", account.objectId);
    group.writeEntry("UserPrincipalName", account.userPrincipalName);
    group.writeEntry("DisplayName", account.displayName);
    group.writeEntry("TenantId", account.tenantId);
    group.writeEntry("Organization", account.organization);
    cfg->sync();
}

GeneralConfig general()
{
    const KConfigGroup group = config()->group(QStringLiteral("General"));
    GeneralConfig general;
    general.baseFolder = group.readEntry("BaseFolder", QDir::homePath());
    general.pauseOnMeteredNetwork = group.readEntry("PauseOnMeteredNetwork", true);
    general.setupDone = group.readEntry("SetupDone", false);
    general.minimumFreeSpaceMB = std::max(0, group.readEntry("MinimumFreeSpaceMB", 1024));
    general.fullScanHours = std::max(0, group.readEntry("FullScanHours", 24));
    general.excludePatterns = group.readEntry("ExcludePatterns", QStringList());
    general.skipDotFiles = group.readEntry("SkipDotFiles", false);
    general.downloadLimitKBps = std::max(0, group.readEntry("DownloadLimitKBps", 0));
    general.uploadLimitKBps = std::max(0, group.readEntry("UploadLimitKBps", 0));
    return general;
}

void setGeneral(const GeneralConfig &general)
{
    auto cfg = config();
    KConfigGroup group = cfg->group(QStringLiteral("General"));
    group.writeEntry("BaseFolder", general.baseFolder);
    group.writeEntry("PauseOnMeteredNetwork", general.pauseOnMeteredNetwork);
    group.writeEntry("SetupDone", general.setupDone);
    group.writeEntry("MinimumFreeSpaceMB", general.minimumFreeSpaceMB);
    group.writeEntry("FullScanHours", general.fullScanHours);
    group.writeEntry("ExcludePatterns", general.excludePatterns);
    group.writeEntry("SkipDotFiles", general.skipDotFiles);
    group.writeEntry("DownloadLimitKBps", general.downloadLimitKBps);
    group.writeEntry("UploadLimitKBps", general.uploadLimitKBps);
    cfg->sync();
}

QList<RootConfig> roots()
{
    QList<RootConfig> result;
    const auto cfg = config();
    const QStringList groups = cfg->groupList();
    for (const QString &name : groups) {
        if (!name.startsWith(rootPrefix)) {
            continue;
        }
        const KConfigGroup group = cfg->group(name);
        RootConfig root;
        root.id = name.mid(rootPrefix.size());
        root.name = group.readEntry("Name", root.id);
        root.localPath = group.readEntry("LocalPath", QString());
        root.driveId = group.readEntry("DriveId", QString());
        root.remoteRootId = group.readEntry("RemoteRootId", QString());
        root.enabled = group.readEntry("Enabled", true);
        root.kind = group.readEntry("Kind", QStringLiteral("library"));
        root.webUrl = group.readEntry("WebUrl", QString());
        root.excludedPaths = group.readEntry("ExcludedPaths", QStringList());
        root.parentRootId = group.readEntry("ParentRoot", QString());
        root.filesOnDemand = group.readEntry("FilesOnDemand", false);
        result.append(root);
    }
    return result;
}

RootPathProblem rootPathProblem(const QString &path)
{
    if (!QDir::isAbsolutePath(path)) {
        return RootPathProblem::NotAbsolute;
    }
    const QString clean = QDir::cleanPath(path);
    const QString home = QDir::cleanPath(QDir::homePath());
    if (clean == QLatin1String("/") || clean == home || home.startsWith(clean + QLatin1Char('/'))) {
        return RootPathProblem::TooBroad;
    }
    const QList<RootConfig> existing = roots();
    for (const RootConfig &root : existing) {
        const QString other = QDir::cleanPath(root.localPath);
        if (clean == other || clean.startsWith(other + QLatin1Char('/')) || other.startsWith(clean + QLatin1Char('/'))) {
            return RootPathProblem::Overlaps;
        }
    }
    return RootPathProblem::None;
}

RootConfig addRoot(const RootConfig &root)
{
    RootConfig stored = root;
    if (stored.id.isEmpty()) {
        stored.id = QString::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, QLatin1Char('0'));
    }
    stored.localPath = QDir::cleanPath(QDir(stored.localPath).absolutePath());

    auto cfg = config();
    KConfigGroup group = cfg->group(rootPrefix + stored.id);
    group.writeEntry("Name", stored.name);
    group.writeEntry("LocalPath", stored.localPath);
    group.writeEntry("DriveId", stored.driveId);
    group.writeEntry("RemoteRootId", stored.remoteRootId);
    group.writeEntry("Enabled", stored.enabled);
    group.writeEntry("Kind", stored.kind);
    group.writeEntry("WebUrl", stored.webUrl);
    group.writeEntry("ExcludedPaths", stored.excludedPaths);
    // Kept as it was when a root is written again (shortcut roots are rewritten after renames)
    if (!group.hasKey("FilesOnDemand") || stored.filesOnDemand) {
        group.writeEntry("FilesOnDemand", stored.filesOnDemand);
    }
    if (stored.parentRootId.isEmpty()) {
        group.deleteEntry("ParentRoot");
    } else {
        group.writeEntry("ParentRoot", stored.parentRootId);
    }
    cfg->sync();
    return stored;
}

bool removeRoot(const QString &id)
{
    auto cfg = config();
    if (!cfg->hasGroup(rootPrefix + id)) {
        return false;
    }
    cfg->deleteGroup(rootPrefix + id);
    cfg->sync();
    QFile::remove(QDir(stateDirectory()).filePath(id + QStringLiteral(".sqlite3")));
    return true;
}

void setExcludedPaths(const QString &id, const QStringList &paths)
{
    auto cfg = config();
    if (!cfg->hasGroup(rootPrefix + id)) {
        return;
    }
    KConfigGroup group = cfg->group(rootPrefix + id);
    group.writeEntry("ExcludedPaths", paths);
    cfg->sync();
}

QString stateDirectory()
{
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)).filePath(QStringLiteral("graphsync"));
    QDir().mkpath(dir);
    // The databases list every synced name and hold upload addresses: owner only, also when the
    // home folder is readable by others
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return dir;
}

QString sharePointFolderName(const QString &organization)
{
    QString name = organization;
    name.replace(QLatin1Char('/'), QLatin1Char('-'));
    return QStringLiteral("SharePoint - ") + name.trimmed();
}

void setFilesOnDemand(const QString &id, bool on)
{
    auto cfg = config();
    if (!cfg->hasGroup(rootPrefix + id)) {
        return;
    }
    KConfigGroup group = cfg->group(rootPrefix + id);
    group.writeEntry("FilesOnDemand", on);
    cfg->sync();
}

}
