// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "auth.h"
#include "config.h"
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
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

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

std::optional<RootConfig> findRoot(const QString &id)
{
    const QList<RootConfig> roots = Config::roots();
    for (const RootConfig &root : roots) {
        if (root.id == id || root.name == id) {
            return root;
        }
    }
    return std::nullopt;
}

int callDaemon(const QString &method, const QVariantList &arguments = {})
{
    QDBusInterface iface(QStringLiteral("io.github.nvaert1986.GraphSync"), QStringLiteral("/GraphSync"), QStringLiteral("io.github.nvaert1986.GraphSync"));
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
  login | logout | whoami                   sign in (browser), sign out, show account
  drives --site-url URL                     list document libraries of a SharePoint site
  add-root --name N --local PATH --drive ID [--remote-root ITEM]
  roots | remove-root ROOT                  list or remove sync roots
  sync ROOT [--download-only]               run one sync cycle in this process
  status | activity | sync-now | pause [MINUTES] | resume   talk to the running graphsyncd
  migrate                                   take over an install of plasma-onedrive-linux (run once at login)
)";
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("graphsyncctl"));
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
        // The same registration in another spelling keeps its sign-in (moved to the new key)
        const AuthConfig stored = Config::auth();
        const AuthConfig wanted = Auth::normalized({clientId, tenantId});
        if (Auth::normalized(stored).clientId == wanted.clientId && Auth::normalized(stored).tenantId == wanted.tenantId
            && Auth::keychainKeyFor(stored) != Auth::keychainKeyFor(wanted)) {
            QString moveError;
            Auth::moveToken(stored, wanted, &moveError);
        }
        Config::setAuth(wanted);
        out() << "stored in ~/.config/graphsyncrc (tenant " << tenantId << ")" << Qt::endl;
        return 0;
    }

    Auth auth(Config::auth());
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
        AccountInfo account = Config::account();
        if (!orgs.isEmpty()) {
            account.organization = orgs.at(0).toObject().value(QLatin1String("displayName")).toString();
            Config::setAccount(account);
        }
        out() << "Signed in as " << account.displayName << " <" << account.userPrincipalName << "> (" << account.organization << ")" << Qt::endl;
        return 0;
    }
    if (command == QLatin1String("logout")) {
        auth.logout();
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
        root.localPath = parser.value(localOpt);
        root.driveId = parser.value(driveOpt);
        root.remoteRootId = parser.value(remoteRootOpt);
        const RootConfig stored = Config::addRoot(root);
        out() << "added root " << stored.id << " (" << stored.name << ") -> " << stored.localPath << Qt::endl;
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
        if (!root || !Config::removeRoot(root->id)) {
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
            return fail(QStringLiteral("no such root"));
        }
        SyncEngine engine(*root, &auth);
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
        QString error;
        auto print = [](const QVariant &value) {
            out() << QJsonDocument::fromVariant(value).toJson(QJsonDocument::Indented);
            return 0;
        };
        if (command == QLatin1String("people")) {
            bool notAllowed = false;
            const QVariantList people = service.searchPeople(args.value(1), &notAllowed, &error);
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
                                     {QStringLiteral("isFolder"), item.isFolder}});
        }
        if (command == QLatin1String("link")) {
            // link <path> [view|edit] [anonymous|organization|users]; no scope = organisation default
            const QVariantMap link = service.createLink(item, args.value(2, QStringLiteral("view")), args.value(3), {}, &error);
            return error.isEmpty() ? print(link) : fail(error);
        }
        if (command == QLatin1String("permissions")) {
            const QVariantList list = service.permissions(item, &error);
            return error.isEmpty() ? print(list) : fail(error);
        }
        if (command == QLatin1String("unshare")) {
            return service.removePermission(item, args.value(2), &error) ? 0 : fail(error);
        }
        if (command == QLatin1String("invite")) {
            // invite <path> <email> [read|write]; no e-mail is sent
            return service.invite(item, {args.value(2)}, args.value(3, QStringLiteral("read")), false, {}, &error) ? 0 : fail(error);
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
            const GraphResponse response = graph.download(QStringLiteral("/drives/%1/items/%2/content").arg(item.driveId, item.id), &target);
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
