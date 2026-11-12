// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "auth.h"
#include "placeholder.h"

#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>

#include <QDir>
#include <QFile>

#include <algorithm>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

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
    return {group.readEntry("ClientId", QString()), group.readEntry("TenantId", QString()), {}};
}

void setAuth(const AuthConfig &given)
{
    const AuthConfig auth{given.clientId.trimmed().toLower(), given.tenantId.trimmed().toLower(), {}};
    auto cfg = config();
    KConfigGroup group = cfg->group(QStringLiteral("Auth"));
    group.writeEntry("ClientId", auth.clientId);
    group.writeEntry("TenantId", auth.tenantId);
    cfg->sync();
}

AccountInfo account()
{
    return GraphCore::Accounts::find(config()->group(QStringLiteral("General")).readEntry("Account", QString()));
}

void setCurrentAccountId(const QString &id)
{
    auto cfg = config();
    KConfigGroup group = cfg->group(QStringLiteral("General"));
    if (id.isEmpty()) {
        group.deleteEntry("Account");
    } else {
        group.writeEntry("Account", id);
    }
    cfg->sync();
}

AccountInfo registerAccount(const AccountInfo &signedIn, bool legacyOwner)
{
    AccountInfo account = signedIn;
    const AccountInfo known = GraphCore::Accounts::findUser(account.tenantId, account.objectId);
    if (!known.id.isEmpty() && account.organization.isEmpty()) {
        account.organization = known.organization;
    }
    // The first registration becomes the suite's default; another one stays with its account, so
    // the other apps' accounts keep theirs
    const QString client = account.clientId;
    if (GraphCore::Accounts::defaultClientId().isEmpty() && !client.isEmpty()) {
        GraphCore::Accounts::setDefaultClientId(client);
    }
    account.clientId = client.compare(GraphCore::Accounts::defaultClientId(), Qt::CaseInsensitive) == 0 ? QString() : client;
    const AccountInfo stored = GraphCore::Accounts::add(account);

    setCurrentAccountId(stored.id);
    // Locations claimed by this sign-in: those of this user whose account entry went (another
    // app removed it), and those set up before there were accounts, if they belong to this
    // organisation (the 0.7 setup's tenant); never another organisation's
    const QString user = accountUserKey(stored);
    // The 0.7 setup's tenant as written: a tenant ID, or a domain (resolved now: this runs right after
    // a sign-in, so online); "organizations" or one that does not resolve proves nothing, and then
    // those locations wait for the user to choose
    QString legacyTenant = legacyOwner ? QString() : auth().tenantId.trimmed().toLower();
    if (!legacyTenant.isEmpty() && !Auth::isGuid(legacyTenant)) {
        const bool generic = legacyTenant == QLatin1String("organizations") || legacyTenant == QLatin1String("common") || legacyTenant == QLatin1String("consumers");
        bool offline = false;
        legacyTenant = generic ? QString() : Auth::resolveTenant(legacyTenant, &offline);
        if (legacyTenant.isEmpty()) {
            legacyTenant = QStringLiteral("-");
        }
    }
    const bool legacyMatches = legacyTenant.isEmpty() || legacyTenant == stored.tenantId.toLower();
    QSet<QString> existing;
    const QList<AccountInfo> accounts = GraphCore::Accounts::all();
    for (const AccountInfo &each : accounts) {
        if (each.isValid()) {
            existing.insert(each.id);
        }
    }
    auto cfg = config();
    const QStringList groups = cfg->groupList();
    for (const QString &name : groups) {
        if (!name.startsWith(rootPrefix)) {
            continue;
        }
        KConfigGroup group = cfg->group(name);
        const QString accountId = group.readEntry("Account", QString());
        const QString accountUser = group.readEntry("AccountUser", QString());
        const bool orphan = accountId.isEmpty() || !existing.contains(accountId);
        if (accountId == stored.id || (orphan && accountUser == user) || (orphan && accountUser.isEmpty() && accountId.isEmpty() && legacyMatches)) {
            group.writeEntry("Account", stored.id);
            group.writeEntry("AccountUser", user);
        }
    }
    cfg->sync();
    return stored;
}

void setAccountOrganization(const QString &id, const QString &organization)
{
    AccountInfo account = GraphCore::Accounts::find(id);
    if (account.isValid() && account.organization != organization) {
        account.organization = organization;
        GraphCore::Accounts::add(account);
    }
}

QString accountUserKey(const AccountInfo &account)
{
    return account.tenantId.toLower() + QLatin1Char(':') + account.objectId.toLower();
}

QString legacyTokenUser()
{
    return config()->group(QStringLiteral("General")).readEntry("LegacyTokenUser", QString());
}

QString legacyTokenKey()
{
    return config()->group(QStringLiteral("General")).readEntry("LegacyTokenKey", QString());
}

void setLegacyTokenUser(const QString &objectId, const QString &key)
{
    auto cfg = config();
    KConfigGroup group = cfg->group(QStringLiteral("General"));
    if (objectId.isEmpty()) {
        group.deleteEntry("LegacyTokenUser");
        group.deleteEntry("LegacyTokenKey");
    } else {
        group.writeEntry("LegacyTokenUser", objectId);
        group.writeEntry("LegacyTokenKey", key);
    }
    cfg->sync();
}

AuthConfig currentAuth()
{
    const AccountInfo current = account();
    return current.isValid() ? authFor(current) : auth();
}

AuthConfig authFor(const AccountInfo &account)
{
    // No suite default yet (graphaccountsrc written by an older version): the one set up here
    const QString client = account.effectiveClientId();
    return {client.isEmpty() ? auth().clientId : client, account.tenantId, account.objectId};
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
        root.accountId = group.readEntry("Account", QString());
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

RootConfig rootForPath(const QString &path)
{
    const QString clean = Placeholder::hasSuffix(path) && Placeholder::isPlaceholder(path) ? Placeholder::realPathOf(QDir::cleanPath(path)) : QDir::cleanPath(path);
    RootConfig owner;
    qsizetype longest = -1;
    const QList<RootConfig> configured = roots();
    for (const RootConfig &each : configured) {
        const QString rootPath = QDir::cleanPath(each.localPath);
        if ((clean == rootPath || clean.startsWith(rootPath + QLatin1Char('/'))) && rootPath.size() > longest) {
            longest = rootPath.size();
            owner = each;
            owner.localPath = rootPath;
        }
    }
    return owner;
}

RootConfig root(const QString &id)
{
    const QList<RootConfig> configured = roots();
    for (const RootConfig &each : configured) {
        if (each.id == id) {
            return each;
        }
    }
    return {};
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
    // Kept when a root is written again without one (shortcut roots after renames)
    if (!stored.accountId.isEmpty()) {
        group.writeEntry("Account", stored.accountId);
        // Who it is, so it finds its account again if the entry in graphaccountsrc goes
        const AccountInfo owner = GraphCore::Accounts::find(stored.accountId);
        if (owner.isValid()) {
            group.writeEntry("AccountUser", accountUserKey(owner));
        }
    }
    stored.accountId = group.readEntry("Account", QString());
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

namespace
{
// "contoso" for contoso.sharepoint.com and contoso-my.sharepoint.com
QString tenantHostPrefix(const QString &host)
{
    QString prefix = host.section(QLatin1Char('.'), 0, 0).toLower();
    if (prefix.endsWith(QLatin1String("-my"))) {
        prefix.chop(3);
    }
    return prefix;
}
}

AccountInfo accountForLink(const QString &userId, const QString &userEmail, const QString &host)
{
    const QList<AccountInfo> accounts = GraphCore::Accounts::all();
    for (const AccountInfo &account : accounts) {
        if (!userId.isEmpty() && account.objectId.compare(userId, Qt::CaseInsensitive) == 0) {
            return account;
        }
    }
    for (const AccountInfo &account : accounts) {
        if (!userEmail.isEmpty() && account.userPrincipalName.compare(userEmail, Qt::CaseInsensitive) == 0) {
            return account;
        }
    }
    const QString prefix = tenantHostPrefix(host);
    const QList<RootConfig> configured = roots();
    for (const RootConfig &root : configured) {
        if (prefix.isEmpty() || tenantHostPrefix(QUrl(root.webUrl).host()) != prefix) {
            continue;
        }
        // A location whose account is gone does not decide it; another one of the same host may
        for (const AccountInfo &account : accounts) {
            if (account.id == root.accountId) {
                return account;
            }
        }
    }
    return account();
}

QString organizationLabel(const AccountInfo &account)
{
    const QString withAddress = QStringLiteral("%1 (%2)").arg(account.organization, account.userPrincipalName);
    bool ownLocations = false;
    bool ownWithAddress = false;
    bool otherUserOfTenant = false;
    // The users of this tenant other than this one (graphaccountsrc read once, not per location)
    QSet<QString> colleagues;
    const QList<AccountInfo> accounts = GraphCore::Accounts::all();
    for (const AccountInfo &other : accounts) {
        if (other.isValid() && other.id != account.id && other.tenantId == account.tenantId) {
            colleagues.insert(other.id);
        }
    }
    const QList<RootConfig> configured = roots();
    for (const RootConfig &root : configured) {
        if (root.accountId.isEmpty()) {
            continue;
        }
        if (root.accountId == account.id) {
            ownLocations = true;
            ownWithAddress = ownWithAddress || root.localPath.contains(QStringLiteral(" (%1)").arg(account.userPrincipalName));
            continue;
        }
        otherUserOfTenant = otherUserOfTenant || colleagues.contains(root.accountId);
    }
    // An account keeps the name its folders already have; a newcomer of an organisation that
    // syncs here already gets its address added
    if (ownLocations) {
        return ownWithAddress ? withAddress : account.organization;
    }
    return otherUserOfTenant ? withAddress : account.organization;
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
