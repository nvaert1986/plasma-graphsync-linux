// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "auth.h"
#include "config.h"
#include "dbusnames.h"
#include "folderbackup.h"
#include "graphclient.h"
#include "itemservice.h"
#include "migration.h"
#include "ratelimiter.h"
#include "syncengine.h"

#include <KConfig>
#include <KConfigGroup>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

#include <algorithm>
#include <map>

using namespace GraphSync;

namespace
{
QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

int fail(const QString &message)
{
    err() << "error: " << message << Qt::endl;
    return 1;
}

// By id; by name only when exactly one location has that name (two accounts can both have
// "OneDrive - Contoso", and acting on the wrong one removes its state)
std::optional<RootConfig> findRoot(const QString &id)
{
    const QList<RootConfig> roots = Config::roots();
    std::optional<RootConfig> byName;
    int named = 0;
    for (const RootConfig &root : roots) {
        if (root.id == id) {
            return root;
        }
        if (root.name == id) {
            byName = root;
            ++named;
        }
    }
    return named == 1 ? byName : std::nullopt;
}

int callDaemon(const QString &method, const QVariantList &arguments = {})
{
    QDBusInterface iface(DBusNames::service(), DBusNames::path(), DBusNames::interfaceName());
    if (!iface.isValid()) {
        return fail(QStringLiteral("graphsyncd is not running"));
    }
    QDBusMessage reply = iface.callWithArgumentList(QDBus::Block, method, arguments);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return fail(reply.errorMessage());
    }
    const QString text = reply.arguments().isEmpty() ? QString() : reply.arguments().constFirst().toString();
    out() << (text.isEmpty() ? QStringLiteral("ok") : text) << Qt::endl;
    return 0;
}

const char usage[] = R"(Commands:
  configure --client-id ID --tenant-id ID   store the app registration
  login [device] | logout | whoami          sign in (browser or code), sign out, show the current account
  accounts                                  list the accounts and their locations (* = current)
  add-account EMAIL [device] [--client-id ID]   sign in another account (another organisation or user);
                                            it becomes the current account
  use-account ACCOUNT                       make an account the current one (new locations, app)
  approval-link EMAIL|DOMAIN [--client-id ID]   the page where an organisation's administrator approves
                                            the app registration
  drives --site-url URL                     list document libraries of a SharePoint site
  add-root --name N --local PATH --drive ID [--remote-root ITEM]
  add-personal [--local PATH]               sync the current account's own OneDrive
  backup-folders desktop|documents|pictures...   PC folder backup: keep these folders in the
                                            current account's OneDrive (stop: remove-root ROOT)
  roots | remove-root ROOT                  list or remove sync roots
  sync ROOT [--download-only]               run one sync cycle in this process
  status | activity | sync-now | pause [MINUTES] | resume   talk to the running graphsyncd
  migrate                                   take over an install of plasma-onedrive-linux (run once at login)
  item | link | permissions | unshare | invite | versions | get-version | restore-version | download |
  add-shortcut PATH ...                     work on a synced file with its location's account
  people TEXT                               name suggestions; details in graphsyncctl(1)
)";
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("graphsyncctl"));
    // Accounts are shared by the suite; the app registration per account is this app's own
    GraphCore::Accounts::setApplication(QStringLiteral("plasma-graphsync-linux"));
    QCoreApplication::setApplicationVersion(QStringLiteral(GRAPHSYNC_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("plasma-graphsync-linux command-line tool\n\n") + QString::fromLatin1(usage));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("Command to run"));
    const QCommandLineOption clientIdOpt(QStringLiteral("client-id"), QStringLiteral("Application (client) ID"), QStringLiteral("id"));
    const QCommandLineOption tenantIdOpt(QStringLiteral("tenant-id"), QStringLiteral("Directory (tenant) ID"), QStringLiteral("id"));
    const QCommandLineOption siteUrlOpt(QStringLiteral("site-url"), QStringLiteral("SharePoint site URL"), QStringLiteral("url"));
    const QCommandLineOption nameOpt(QStringLiteral("name"), QStringLiteral("Display name of the sync root"), QStringLiteral("name"));
    const QCommandLineOption localOpt(QStringLiteral("local"), QStringLiteral("Local folder"), QStringLiteral("path"));
    const QCommandLineOption driveOpt(QStringLiteral("drive"), QStringLiteral("Drive (library) ID"), QStringLiteral("id"));
    const QCommandLineOption remoteRootOpt(QStringLiteral("remote-root"), QStringLiteral("Item ID of a folder to sync instead of the whole library"),
                                           QStringLiteral("item"));
    const QCommandLineOption downloadOnlyOpt(QStringLiteral("download-only"), QStringLiteral("Only apply online changes locally"));
    parser.addOptions({clientIdOpt, tenantIdOpt, siteUrlOpt, nameOpt, localOpt, driveOpt, remoteRootOpt, downloadOnlyOpt});
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        parser.showHelp(1);
    }
    const QString command = args.constFirst();

    // An install of plasma-onedrive-linux is taken over before any setting is read or written: a
    // write would otherwise start a new setup next to it
    QString migration;
    const Migration::Result imported = Migration::importLegacyInstall(&migration);
    if (imported != Migration::Result::Busy) {
        Migration::adoptSharedAccounts();
    }
    if (command == QLatin1String("migrate")) {
        // Started once per user at login (autostart entry): starts the sync service when it took
        // over, and then switches its own autostart off for this user. Busy or failed: the entry
        // stays, so the next login tries again
        if (imported == Migration::Result::Busy || imported == Migration::Result::Failed) {
            return fail(migration);
        }
        if (imported == Migration::Result::Imported) {
            out() << migration << Qt::endl;
            QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("start"), QStringLiteral("plasma-graphsync-linux.service")});
        }
        KConfig autostart(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                              + QStringLiteral("/autostart/io.github.nvaert1986.graphsync.migrate.desktop"),
                          KConfig::SimpleConfig);
        KConfigGroup entry = autostart.group(QStringLiteral("Desktop Entry"));
        entry.writeEntry("Type", QStringLiteral("Application"));
        entry.writeEntry("Name", QStringLiteral("plasma-graphsync-linux: take over plasma-onedrive-linux"));
        entry.writeEntry("Exec", QStringLiteral("graphsyncctl migrate"));
        entry.writeEntry("Hidden", true);
        return 0;
    }
    if (imported == Migration::Result::Busy || imported == Migration::Result::Failed) {
        QTextStream(stderr) << "warning: taking over plasma-onedrive-linux: " << migration << Qt::endl;
    }

    // Transfers from here follow the same bandwidth limits as the sync service
    const GeneralConfig limits = Config::general();
    RateLimiter::setLimits(qint64(limits.downloadLimitKBps) * 1024, qint64(limits.uploadLimitKBps) * 1024);

    if (command == QLatin1String("configure")) {
        if (!parser.isSet(clientIdOpt) || !parser.isSet(tenantIdOpt)) {
            return fail(QStringLiteral("configure needs --client-id and --tenant-id"));
        }
        // The tenant may also be given as a domain or a work e-mail address
        bool offline = false;
        const QString tenantId = Auth::resolveTenant(parser.value(tenantIdOpt), &offline);
        if (tenantId.isEmpty()) {
            return fail(offline ? QStringLiteral("no connection to Microsoft 365") : QStringLiteral("no Microsoft 365 organization found for ") + parser.value(tenantIdOpt));
        }
        const QString clientId = parser.value(clientIdOpt).trimmed().toLower();
        if (!Auth::isGuid(clientId)) {
            return fail(QStringLiteral("--client-id must look like 00000000-0000-0000-0000-000000000000"));
        }
        // For signing in new accounts; an account keeps the registration it signed in with (IDs are
        // stored in one spelling, so there is nothing to move)
        Config::setAuth(Auth::normalized({clientId, tenantId, {}}));
        out() << "stored in ~/.config/graphsyncrc (tenant " << tenantId << ") for new sign-ins; signed-in accounts keep their own"
              << Qt::endl;
        return 0;
    }

    if (command == QLatin1String("accounts")) {
        const QString current = Config::account().id;
        const QList<RootConfig> roots = Config::roots();
        const QList<AccountInfo> accounts = GraphCore::Accounts::all();
        for (const AccountInfo &account : accounts) {
            const auto locations = std::count_if(roots.cbegin(), roots.cend(), [&account](const RootConfig &root) {
                return root.accountId == account.id && root.kind != QLatin1String("shortcut");
            });
            out() << (account.id == current ? "* " : "  ") << account.id << "  " << account.userPrincipalName << "  (" << account.organization << ")  "
                  << locations << " location(s)" << (account.clientId.isEmpty() ? QString() : QStringLiteral("  client ") + account.clientId) << Qt::endl;
        }
        const auto orphans = std::count_if(roots.cbegin(), roots.cend(), [](const RootConfig &root) {
            return root.accountId.isEmpty();
        });
        if (orphans > 0) {
            out() << "  " << orphans << " location(s) without an account: the next sign-in claims them" << Qt::endl;
        }
        return 0;
    }
    if (command == QLatin1String("use-account")) {
        const AccountInfo account = GraphCore::Accounts::find(args.value(1));
        if (!account.isValid()) {
            return fail(QStringLiteral("no account ") + args.value(1) + QStringLiteral(" (see graphsyncctl accounts)"));
        }
        Config::setCurrentAccountId(account.id);
        // Only the pointer changes: no restart of the running locations
        QDBusInterface(DBusNames::service(), DBusNames::path(), DBusNames::interfaceName())
            .call(QStringLiteral("Command"), QStringLiteral(R"({"command":"setCurrentAccount","id":"%1"})").arg(account.id));
        out() << "current account: " << account.userPrincipalName << Qt::endl;
        return 0;
    }
    if (command == QLatin1String("approval-link") || command == QLatin1String("add-account")) {
        const QString address = args.value(1).trimmed();
        if (address.isEmpty()) {
            return fail(command + QStringLiteral(" needs a work e-mail address or domain"));
        }
        const QString defaultClient = GraphCore::Accounts::defaultClientId().isEmpty() ? Config::auth().clientId : GraphCore::Accounts::defaultClientId();
        const QString client = parser.isSet(clientIdOpt) ? parser.value(clientIdOpt).trimmed().toLower() : defaultClient;
        if (!Auth::isGuid(client)) {
            return fail(QStringLiteral("no app registration: pass --client-id 00000000-0000-0000-0000-000000000000"));
        }
        bool offline = false;
        const QString tenant = Auth::resolveTenant(address, &offline);
        if (tenant.isEmpty()) {
            return fail(offline ? QStringLiteral("no connection to Microsoft 365") : QStringLiteral("no Microsoft 365 organization found for ") + address);
        }
        if (command == QLatin1String("approval-link")) {
            out() << Auth::adminConsentUrl(tenant, client).toString() << Qt::endl;
            return 0;
        }
        Auth added({client, tenant, {}});
        QString error;
        bool ok = false;
        if (!Auth::browserLoginAvailable() || args.value(2) == QLatin1String("device")) {
            ok = added.deviceCodeLogin(
                [](const QString &code, const QString &page) {
                    out() << "Open " << page << " on any device and enter the code " << code << Qt::endl;
                },
                nullptr,
                &error);
        } else {
            out() << "Opening the browser for sign-in..." << Qt::endl;
            ok = added.interactiveLogin(&error, address.contains(QLatin1Char('@')) ? address : QString());
        }
        if (!ok) {
            if (Auth::needsAdminConsent(error)) {
                out() << "The organization has not approved the app yet. Its administrator opens this page once:" << Qt::endl
                      << Auth::adminConsentUrl(tenant, client).toString() << Qt::endl;
            }
            return fail(error);
        }
        GraphClient graph(&added);
        const QJsonArray orgs = graph.get(QStringLiteral("/organization?$select=displayName")).json().value(QLatin1String("value")).toArray();
        if (!orgs.isEmpty()) {
            Config::setAccountOrganization(Config::account().id, orgs.at(0).toObject().value(QLatin1String("displayName")).toString());
        }
        const AccountInfo account = Config::account();
        // Only this account's locations restart in the running daemon
        QDBusInterface(DBusNames::service(), DBusNames::path(), DBusNames::interfaceName())
            .call(QStringLiteral("Command"), QStringLiteral(R"({"command":"reloadAccount","id":"%1"})").arg(account.id));
        out() << "Added " << account.displayName << " <" << account.userPrincipalName << "> (" << account.organization << ") as account " << account.id
              << "; it is now the current account" << Qt::endl;
        return 0;
    }

    Auth auth(Config::currentAuth());
    if (!auth.isConfigured() && command != QLatin1String("roots")) {
        return fail(QStringLiteral("run graphsyncctl configure first"));
    }

    if (command == QLatin1String("login")) {
        QString error;
        if (!Auth::browserLoginAvailable() || args.contains(QStringLiteral("--device")) || args.value(1) == QLatin1String("device")) {
            // login device: for a remote session or when the browser cannot come back to us (and
            // the only sign-in on builds older than Qt 6.9)
            const bool ok = auth.deviceCodeLogin(
                [](const QString &code, const QString &address) {
                    out() << "Open " << address << " on any device and enter the code " << code << Qt::endl;
                },
                nullptr,
                &error);
            if (!ok) {
                return fail(error);
            }
        } else {
            out() << "Opening the browser for sign-in..." << Qt::endl;
            if (!auth.interactiveLogin(&error)) {
                return fail(error);
            }
        }
        GraphClient graph(&auth);
        const QJsonArray orgs = graph.get(QStringLiteral("/organization?$select=displayName")).json().value(QLatin1String("value")).toArray();
        if (!orgs.isEmpty()) {
            Config::setAccountOrganization(Config::account().id, orgs.at(0).toObject().value(QLatin1String("displayName")).toString());
        }
        const AccountInfo account = Config::account();
        out() << "Signed in as " << account.displayName << " <" << account.userPrincipalName << "> (" << account.organization << ")" << Qt::endl;
        // A running daemon starts this account's locations
        QDBusInterface(DBusNames::service(), DBusNames::path(), DBusNames::interfaceName())
            .call(QStringLiteral("Command"), QStringLiteral(R"({"command":"reloadAccount","id":"%1"})").arg(account.id));
        return 0;
    }
    if (command == QLatin1String("logout")) {
        // Through the running daemon: it stops the account's locations (they would keep syncing
        // with the token in its memory) and picks another current account
        const QString accountId = Config::account().id;
        QDBusInterface daemon(DBusNames::service(), DBusNames::path(), DBusNames::interfaceName());
        const QJsonObject request{{QStringLiteral("command"), QStringLiteral("signOutAccount")}, {QStringLiteral("id"), accountId}};
        if (accountId.isEmpty() || !daemon.isValid() || !daemon.call(QStringLiteral("Command"), QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact))).errorMessage().isEmpty()) {
            auth.logout();
        }
        out() << "signed out; the refresh token was removed from the keychain" << Qt::endl;
        return 0;
    }
    if (command == QLatin1String("whoami")) {
        GraphClient graph(&auth);
        const GraphResponse me = graph.get(QStringLiteral("/me?$select=displayName,userPrincipalName"));
        if (!me.ok()) {
            return fail(me.describe());
        }
        const GraphResponse drive = graph.get(QStringLiteral("/me/drive?$select=id,driveType,quota"));
        const QJsonObject quota = drive.json().value(QLatin1String("quota")).toObject();
        out() << me.json().value(QLatin1String("displayName")).toString() << " <" << me.json().value(QLatin1String("userPrincipalName")).toString()
              << ">" << Qt::endl;
        out() << "OneDrive: " << drive.json().value(QLatin1String("driveType")).toString() << ", "
              << QString::number(quota.value(QLatin1String("used")).toDouble() / 1e9, 'f', 1) << " GB of "
              << QString::number(quota.value(QLatin1String("total")).toDouble() / 1e9, 'f', 0) << " GB used" << Qt::endl;
        return 0;
    }
    if (command == QLatin1String("drives")) {
        const QUrl site(parser.value(siteUrlOpt));
        if (!site.isValid() || site.host().isEmpty()) {
            return fail(QStringLiteral("drives needs --site-url https://tenant.sharepoint.com/sites/name"));
        }
        GraphClient graph(&auth);
        const GraphResponse siteInfo = graph.get(QStringLiteral("/sites/%1:%2?$select=id,displayName").arg(site.host(), site.path()));
        if (!siteInfo.ok()) {
            return fail(siteInfo.describe());
        }
        const QString siteId = siteInfo.json().value(QLatin1String("id")).toString();
        const GraphResponse drives = graph.get(QStringLiteral("/sites/%1/drives?$select=id,name,webUrl").arg(siteId));
        if (!drives.ok()) {
            return fail(drives.describe());
        }
        const QJsonArray values = drives.json().value(QLatin1String("value")).toArray();
        for (const QJsonValue &value : values) {
            const QJsonObject d = value.toObject();
            out() << d.value(QLatin1String("name")).toString() << "\t" << d.value(QLatin1String("id")).toString() << "\t"
                  << d.value(QLatin1String("webUrl")).toString() << Qt::endl;
        }
        return 0;
    }
    if (command == QLatin1String("add-root")) {
        if (!parser.isSet(localOpt) || !parser.isSet(driveOpt)) {
            return fail(QStringLiteral("add-root needs --local and --drive"));
        }
        RootConfig root;
        root.name = parser.isSet(nameOpt) ? parser.value(nameOpt) : parser.value(localOpt).section(QLatin1Char('/'), -1);
        root.localPath = QFileInfo(parser.value(localOpt)).absoluteFilePath();
        root.driveId = parser.value(driveOpt);
        root.remoteRootId = parser.value(remoteRootOpt);
        // Syncs with the current account
        root.accountId = Config::account().id;
        // The same check as the app and the daemon: never the home folder, / or a folder inside,
        // around or the same as another location
        if (Config::rootPathProblem(root.localPath) != Config::RootPathProblem::None) {
            return fail(QStringLiteral("%1 cannot be a location: it is not a full path, too broad (home folder or /), or inside or around another location").arg(root.localPath));
        }
        QDBusInterface daemon(DBusNames::service(), DBusNames::path(), DBusNames::interfaceName());
        if (daemon.isValid()) {
            // Through the running daemon, so it starts syncing at once
            const QJsonObject request{{QStringLiteral("name"), root.name},
                                      {QStringLiteral("localPath"), root.localPath},
                                      {QStringLiteral("driveId"), root.driveId},
                                      {QStringLiteral("remoteRootId"), root.remoteRootId},
                                      {QStringLiteral("kind"), root.remoteRootId.isEmpty() ? QStringLiteral("library") : QStringLiteral("folder")},
                                      {QStringLiteral("accountId"), root.accountId}};
            const QDBusReply<QString> added = daemon.call(QStringLiteral("AddRoot"), QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
            if (!added.isValid() || added.value().isEmpty()) {
                return fail(QStringLiteral("the sync service refused ") + root.localPath);
            }
            out() << "added root " << added.value() << " (" << root.name << ") -> " << root.localPath << Qt::endl;
            return 0;
        }
        const RootConfig stored = Config::addRoot(root);
        out() << "added root " << stored.id << " (" << stored.name << ") -> " << stored.localPath << Qt::endl;
        return 0;
    }
    if (command == QLatin1String("add-personal")) {
        // The current account's own OneDrive, as the app's "Sync OneDrive - <Organisation>" does
        const AccountInfo account = Config::account();
        if (!account.isValid()) {
            return fail(QStringLiteral("not signed in (graphsyncctl login or add-account)"));
        }
        GraphClient graph(&auth);
        const GraphResponse drive = graph.get(QStringLiteral("/me/drive?$select=id,webUrl"));
        if (!drive.ok()) {
            return fail(drive.describe());
        }
        QString label = Config::organizationLabel(account);
        label.replace(QLatin1Char('/'), QLatin1Char('-'));
        const QJsonObject root{{QStringLiteral("name"), QString(QStringLiteral("OneDrive - ") + account.organization)},
                               {QStringLiteral("localPath"),
                                parser.isSet(localOpt) ? QFileInfo(parser.value(localOpt)).absoluteFilePath()
                                                       : QDir(Config::general().baseFolder).filePath(QString(QStringLiteral("OneDrive - ") + label))},
                               {QStringLiteral("driveId"), drive.json().value(QLatin1String("id")).toString()},
                               {QStringLiteral("webUrl"), drive.json().value(QLatin1String("webUrl")).toString()},
                               {QStringLiteral("kind"), QStringLiteral("personal")},
                               {QStringLiteral("accountId"), account.id}};
        // Through the daemon: it checks the folder, adds the Places entry and starts at once
        QDBusInterface daemon(DBusNames::service(), DBusNames::path(), DBusNames::interfaceName());
        const QDBusReply<QString> added = daemon.call(QStringLiteral("AddRoot"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
        if (!added.isValid() || added.value().isEmpty()) {
            return fail(added.isValid() ? QStringLiteral("the sync service refused ") + root.value(QLatin1String("localPath")).toString()
                                        : QStringLiteral("graphsyncd is not running"));
        }
        out() << "added root " << added.value() << " -> " << root.value(QLatin1String("localPath")).toString() << Qt::endl;
        return 0;
    }
    if (command == QLatin1String("backup-folders")) {
        // PC folder backup with the current account: backup-folders desktop|documents|pictures...
        // [--local PATH] (one folder: another local folder than the XDG one, for tests)
        const AccountInfo account = Config::account();
        if (!account.isValid()) {
            return fail(QStringLiteral("not signed in (graphsyncctl login or add-account)"));
        }
        const QStringList keys = args.mid(1);
        if (keys.isEmpty() || (parser.isSet(localOpt) && keys.size() != 1)) {
            return fail(QStringLiteral("backup-folders needs desktop, documents and/or pictures (--local with one folder only)"));
        }
        GraphClient graph(&auth);
        for (const QString &key : keys) {
            auto folder = FolderBackup::folder(key);
            if (!folder) {
                return fail(QStringLiteral("unknown folder %1 (desktop, documents or pictures)").arg(key));
            }
            if (parser.isSet(localOpt)) {
                folder->localPath = QFileInfo(parser.value(localOpt)).absoluteFilePath();
            }
            QString error;
            const QString id = FolderBackup::start(&graph, account.id, *folder, &error);
            if (id.isEmpty()) {
                return fail(error);
            }
            out() << "backing up " << folder->localPath << " (root " << id << ")" << Qt::endl;
        }
        return 0;
    }
    if (command == QLatin1String("roots")) {
        const QList<RootConfig> roots = Config::roots();
        for (const RootConfig &root : roots) {
            out() << root.id << "\t" << root.name << "\t" << root.localPath << "\t" << root.driveId
                  << (root.remoteRootId.isEmpty() ? QString() : QStringLiteral(" (folder %1)").arg(root.remoteRootId))
                  << (root.enabled ? QString() : QStringLiteral(" [disabled]")) << Qt::endl;
        }
        return 0;
    }
    if (command == QLatin1String("remove-root")) {
        if (args.size() < 2) {
            return fail(QStringLiteral("remove-root needs a root id"));
        }
        const auto root = findRoot(args.at(1));
        if (!root) {
            return fail(QStringLiteral("no such root"));
        }
        // Through the running daemon: it stops the location at once (a config edit alone leaves it
        // syncing until the next reload)
        QDBusInterface daemon(DBusNames::service(), DBusNames::path(), DBusNames::interfaceName());
        const QDBusReply<bool> removed = daemon.isValid() ? daemon.call(QStringLiteral("RemoveRoot"), root->id) : QDBusReply<bool>();
        if (!(removed.isValid() && removed.value()) && !Config::removeRoot(root->id)) {
            return fail(QStringLiteral("no such root"));
        }
        out() << "removed root " << root->id << "; local files were left in place" << Qt::endl;
        return 0;
    }
    if (command == QLatin1String("sync")) {
        if (args.size() < 2) {
            return fail(QStringLiteral("sync needs a root id"));
        }
        const auto root = findRoot(args.at(1));
        if (!root) {
            return fail(QStringLiteral("no such root (or a name more than one location has: use the id)"));
        }
        // Never next to the running daemon on the same location: two engines on one state
        // database and one folder race on every move and delete
        {
            QDBusInterface daemon(DBusNames::service(), DBusNames::path(), DBusNames::interfaceName());
            const QDBusReply<QString> status = daemon.isValid() ? daemon.call(QStringLiteral("Status")) : QDBusReply<QString>();
            const QJsonArray running = QJsonDocument::fromJson(status.value().toUtf8()).object().value(QLatin1String("roots")).toArray();
            const bool busy = std::any_of(running.begin(), running.end(), [&](const QJsonValue &each) {
                return each.toObject().value(QLatin1String("id")).toString() == root->id
                    && each.toObject().value(QLatin1String("state")).toString() != QLatin1String("signedOut");
            });
            if (busy) {
                return fail(QStringLiteral("graphsyncd syncs this location already; use `graphsyncctl sync-now`, or stop the service first"));
            }
        }
        // With the location's own account: another account's sign-in would upload as that user
        // and take items it cannot see for deleted
        const AccountInfo owner = GraphCore::Accounts::find(root->accountId);
        if (!owner.isValid()) {
            return fail(QStringLiteral("this location has no account on this computer (graphsyncctl accounts)"));
        }
        Auth ownerAuth(Config::authFor(owner));
        SyncEngine engine(*root, &ownerAuth);
        if (parser.isSet(downloadOnlyOpt)) {
            engine.setMode(SyncEngine::Mode::DownloadOnly);
        }
        QObject::connect(&engine, &SyncEngine::activity, &engine, [](const QString &message) {
            out() << "  " << message << Qt::endl;
        });
        QElapsedTimer timer;
        timer.start();
        QString error;
        const bool ok = engine.syncOnce(&error);
        const SyncEngine::Stats s = engine.lastStats();
        out() << QStringLiteral("delta: %1 page(s), %2 item(s); downloaded %3, uploaded %4, folders created %5, moved local %6 / online %7, "
                                "deleted local %8 / online %9, conflicts %10; %11 s")
                     .arg(s.deltaPages)
                     .arg(s.deltaItems)
                     .arg(s.downloaded)
                     .arg(s.uploaded)
                     .arg(s.foldersCreated)
                     .arg(s.movedLocal)
                     .arg(s.movedRemote)
                     .arg(s.deletedLocal)
                     .arg(s.deletedRemote)
                     .arg(s.conflicts)
                     .arg(timer.elapsed() / 1000.0, 0, 'f', 1)
              << Qt::endl;
        if (!error.isEmpty()) {
            err() << error << Qt::endl;
        }
        return ok && error.isEmpty() ? 0 : 2;
    }
    if (command == QLatin1String("status")) {
        return callDaemon(QStringLiteral("Status"));
    }
    if (command == QLatin1String("activity")) {
        return callDaemon(QStringLiteral("Activity"));
    }
    if (command == QLatin1String("sync-now")) {
        return callDaemon(QStringLiteral("SyncNow"));
    }
    if (command == QLatin1String("pause")) {
        // optional minutes; without it, pause until resumed
        return callDaemon(QStringLiteral("Pause"), {args.size() > 1 ? args.at(1).toInt() : 0});
    }
    if (command == QLatin1String("resume")) {
        return callDaemon(QStringLiteral("Resume"));
    }

    // Sharing and versions of a synced item (the same calls the app's dialogs make)
    static const QStringList itemCommands{QStringLiteral("item"),
                                          QStringLiteral("link"),
                                          QStringLiteral("permissions"),
                                          QStringLiteral("unshare"),
                                          QStringLiteral("invite"),
                                          QStringLiteral("versions"),
                                          QStringLiteral("get-version"),
                                          QStringLiteral("restore-version"),
                                          QStringLiteral("add-shortcut"),
                                          QStringLiteral("download")};
    if (itemCommands.contains(command) || command == QLatin1String("people")) {
        GraphClient graph(&auth);
        ItemService service(&graph);
        // A path in a location of another account goes through that account's sign-in
        std::map<QString, std::pair<std::unique_ptr<Auth>, std::unique_ptr<GraphClient>>> others;
        const auto clientFor = [&others, &graph](const QString &accountId) -> GraphClient * {
            const AccountInfo account = GraphCore::Accounts::find(accountId);
            if (!account.isValid()) {
                return nullptr;
            }
            if (account.id == Config::account().id) {
                return &graph;
            }
            auto &client = others[accountId];
            if (!client.first) {
                client.first = std::make_unique<Auth>(Config::authFor(account));
                client.second = std::make_unique<GraphClient>(client.first.get());
            }
            return client.second.get();
        };
        service.setGraphProvider(clientFor);
        QString error;
        auto print = [](const QVariant &value) {
            out() << QJsonDocument::fromVariant(value).toJson(QJsonDocument::Indented);
            return 0;
        };
        if (command == QLatin1String("people")) {
            bool notAllowed = false;
            const QVariantList people = service.searchPeople(args.value(1), Config::account().id, &notAllowed, &error);
            if (notAllowed) {
                return fail(QStringLiteral("not allowed: the app registration lacks People.Read / User.ReadBasic.All"));
            }
            return error.isEmpty() ? print(people) : fail(error);
        }
        if (args.size() < 2) {
            return fail(QStringLiteral("%1 needs a local path").arg(command));
        }
        const ItemService::Item item = service.resolve(QFileInfo(args.at(1)).absoluteFilePath(), &error);
        if (!item.valid()) {
            return fail(error);
        }
        if (command == QLatin1String("item")) {
            return print(QVariantMap{{QStringLiteral("driveId"), item.driveId},
                                     {QStringLiteral("id"), item.id},
                                     {QStringLiteral("name"), item.name},
                                     {QStringLiteral("webUrl"), item.webUrl},
                                     {QStringLiteral("permissionsUrl"), item.permissionsUrl},
                                     {QStringLiteral("accountId"), item.accountId},
                                     {QStringLiteral("isFolder"), item.isFolder}});
        }
        // The running daemon reads the item's sharing again for Dolphin's emblem (Graph's change
        // feed does not report it); without a daemon the next full check picks it up
        const auto sharingChanged = [&args] {
            QDBusInterface daemon(DBusNames::service(), DBusNames::path(), DBusNames::interfaceName());
            if (daemon.isValid()) {
                const QJsonObject request{{QStringLiteral("command"), QStringLiteral("sharingChanged")},
                                          {QStringLiteral("path"), QFileInfo(args.at(1)).absoluteFilePath()}};
                daemon.call(QStringLiteral("Command"), QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
            }
        };
        if (command == QLatin1String("link")) {
            // link <path> [view|edit] [anonymous|organization|users]; no scope = organisation default
            const QVariantMap link = service.createLink(item, args.value(2, QStringLiteral("view")), args.value(3), {}, &error);
            if (error.isEmpty()) {
                sharingChanged();
            }
            return error.isEmpty() ? print(link) : fail(error);
        }
        if (command == QLatin1String("permissions")) {
            const QVariantList list = service.permissions(item, &error);
            return error.isEmpty() ? print(list) : fail(error);
        }
        if (command == QLatin1String("unshare")) {
            if (!service.removePermission(item, args.value(2), &error)) {
                return fail(error);
            }
            sharingChanged();
            return 0;
        }
        if (command == QLatin1String("invite")) {
            // invite <path> <email> [read|write]; no e-mail is sent
            if (!service.invite(item, {args.value(2)}, args.value(3, QStringLiteral("read")), false, {}, &error)) {
                return fail(error);
            }
            sharingChanged();
            return 0;
        }
        if (command == QLatin1String("versions")) {
            const QVariantList list = service.versions(item, &error);
            return error.isEmpty() ? print(list) : fail(error);
        }
        if (command == QLatin1String("download")) {
            // download <synced file> <target>: the current content (for tests and support)
            QFile target(args.value(2));
            if (item.isFolder || !target.open(QIODevice::WriteOnly)) {
                return fail(QStringLiteral("download needs a file and a writable target"));
            }
            // With the account of the file's location, like every other item command
            GraphClient *client = clientFor(item.accountId);
            if (!client) {
                return fail(QStringLiteral("the account of this location is no longer on this computer"));
            }
            const GraphResponse response = client->download(QStringLiteral("/drives/%1/items/%2/content").arg(item.driveId, item.id), &target);
            return response.ok() ? 0 : fail(response.describe());
        }
        if (command == QLatin1String("add-shortcut")) {
            // add-shortcut <synced folder>: "Add shortcut to My files"
            if (!item.isFolder) {
                return fail(QStringLiteral("add-shortcut needs a folder"));
            }
            const QString name = service.addShortcut(item, &error);
            if (name.isEmpty()) {
                return fail(error);
            }
            out() << "Added shortcut " << name << Qt::endl;
            return 0;
        }
        if (command == QLatin1String("get-version")) {
            return service.downloadVersion(item, args.value(2), args.value(3), &error) ? 0 : fail(error);
        }
        return service.restoreVersion(item, args.value(2), &error) ? 0 : fail(error);
    }
    return fail(QStringLiteral("unknown command %1; see --help").arg(command));
}
