// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace GraphSync
{

// One synced library or library folder, stored as a [Root <id>] group in graphsyncrc.
struct RootConfig {
    QString id;
    QString name;
    QString localPath;
    QString driveId;
    // Empty means the root of the drive; otherwise the item id of the synced folder.
    QString remoteRootId;
    bool enabled = true;
    // "personal" (the user's own OneDrive), "library", "folder" or "shortcut" (an "Add shortcut
    // to My files" folder inside another root, managed by that root's engine)
    QString kind = QStringLiteral("library");
    // For shortcuts: the root whose folder holds this one
    QString parentRootId;
    // Files On-Demand: new files from online arrive as placeholders (<name>.cloud) and are only
    // downloaded when opened or pinned
    bool filesOnDemand = false;
    // Opened by "View online"
    QString webUrl;
    // Folders (relative paths) the user chose not to sync
    QStringList excludedPaths;
};

struct AuthConfig {
    QString clientId;
    QString tenantId;
};

// Signed-in account details, kept so odopen links can be matched to the account.
struct AccountInfo {
    QString objectId;
    QString userPrincipalName;
    QString displayName;
    QString tenantId;
    // Tenant display name, used for "OneDrive - <Organisation>" and the folder layout
    QString organization;
};

struct GeneralConfig {
    // Libraries go to <baseFolder>/SharePoint - <Organisation>/<Site> - <Library>
    QString baseFolder;
    bool pauseOnMeteredNetwork = true;
    bool setupDone = false;
    // Downloads stop while less than this is free on the disk
    int minimumFreeSpaceMB = 1024;
    // Re-list everything this often as a safety net next to the change feed; 0 = never
    int fullScanHours = 24;
    // Names (wildcards, e.g. *.tmp, node_modules) never synced, in either direction
    QStringList excludePatterns;
    // Names starting with a dot are not synced
    bool skipDotFiles = false;
    // Bandwidth limits for all transfers together, in KB/s; 0 = no limit
    int downloadLimitKBps = 0;
    int uploadLimitKBps = 0;
};

namespace Config
{
AuthConfig auth();
void setAuth(const AuthConfig &auth);

AccountInfo account();
void setAccount(const AccountInfo &account);

GeneralConfig general();
void setGeneral(const GeneralConfig &general);

QList<RootConfig> roots();
RootConfig addRoot(const RootConfig &root);

// Why a local folder may not become a sync location (None: it may). Syncing / or the home
// folder would upload everything in it, an overlap would sync files twice
enum class RootPathProblem {
    None,
    NotAbsolute,
    TooBroad,
    Overlaps,
};
RootPathProblem rootPathProblem(const QString &path);
bool removeRoot(const QString &id);
void setExcludedPaths(const QString &id, const QStringList &paths);
void setFilesOnDemand(const QString &id, bool on);

// Per-root SQLite state lives here, outside the synced folder.
QString stateDirectory();

// The folder that holds all SharePoint libraries: <base>/SharePoint - <Organisation>/<Site> - <Library>,
// next to <base>/OneDrive - <Organisation>. Not translated: the daemon recognises the folder by it.
QString sharePointFolderName(const QString &organization);
}

}
