// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "accounts.h"

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
    // The account (GraphCore::Account id in graphaccountsrc) this location syncs with; empty for
    // a location set up while signed out, until an account signs in and claims it
    QString accountId;
    // PC folder backup: "desktop", "documents" or "pictures" for that folder of this computer kept
    // in the account's OneDrive (kind "folder"); empty for other locations
    QString backup;
};

// The app registration and tenant a sign-in uses. objectId is the signed-in user (empty before
// the first sign-in); with it the KWallet entry is per user, so accounts of one tenant and one
// registration keep apart.
struct AuthConfig {
    QString clientId;
    QString tenantId;
    QString objectId;
};

// A signed-in account (shared with the other apps of the suite through graphaccountsrc)
using AccountInfo = GraphCore::Account;

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
    // Files On-Demand: files not opened for this many days become online-only again; 0 = never
    int freeUpAfterDays = 0;
};

namespace Config
{
// What the setup wizard or `graphsyncctl configure` chose for signing in a new account: the app
// registration and the tenant ([Auth] in graphsyncrc, which a package or IT may also ship in
// /etc/xdg/graphsyncrc)
AuthConfig auth();
void setAuth(const AuthConfig &auth);

// The account the app works with (signing in, adding libraries, the Sync button). Every location
// syncs with its own account; until the Accounts page this is the one signed in last. Empty
// account (no id) when signed out.
AccountInfo account();
void setCurrentAccountId(const QString &id);
// Stores a signed-in account in graphaccountsrc (a known user keeps its id and organisation),
// makes it the current one and hands it the locations that have no account yet
// legacyOwner: the account is the one of the 0.7 setup itself (the migration), which claims all of
// its locations; a sign-in only claims them for the same tenant
AccountInfo registerAccount(const AccountInfo &signedIn, bool legacyOwner = false);
// Organisation name from Graph, kept with the account
void setAccountOrganization(const QString &id, const QString &organization);
// The sign-in settings of an account: its app registration, tenant and user
AuthConfig authFor(const AccountInfo &account);
// The current account's sign-in settings, or auth() for a new sign-in when there is none
AuthConfig currentAuth();
// The user (object id) whose sign-in 0.7.x stored under the registration's KWallet key (without
// the user); only that account may move it to its own entry. Empty once moved.
// "tenant:user" of an account, kept with each location (AccountUser)
QString accountUserKey(const AccountInfo &account);
QString legacyTokenUser();
// The exact KWallet key 0.7.x used (its [Auth] values as written, any spelling)
QString legacyTokenKey();
// Empty objectId: the old sign-in was moved or removed
void setLegacyTokenUser(const QString &objectId, const QString &key = {});

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
// path is a location, lies inside one or holds one
bool overlapsRoot(const QString &path);
// The location holding path (the longest match; a Files On-Demand placeholder stands for its real
// file); id empty when none
RootConfig rootForPath(const QString &path);
// The location with this id; id empty when there is none
RootConfig root(const QString &id);
bool removeRoot(const QString &id);
void setExcludedPaths(const QString &id, const QStringList &paths);
void setFilesOnDemand(const QString &id, bool on);

// Per-root SQLite state lives here, outside the synced folder.
QString stateDirectory();

// The folder that holds all SharePoint libraries: <base>/SharePoint - <Organisation>/<Site> - <Library>,
// next to <base>/OneDrive - <Organisation>. Not translated: the daemon recognises the folder by it.
QString sharePointFolderName(const QString &organization);
// The account a SharePoint Sync button link is for: the user it names (object id, else address),
// else an account that already syncs from that SharePoint organisation (host), else the current
// account
AccountInfo accountForLink(const QString &userId, const QString &userEmail, const QString &host);

// The organisation as folder names show it: its name, with the account's address added when
// another account of the same organisation syncs here already (two users of one tenant)
QString organizationLabel(const AccountInfo &account);
}

}
