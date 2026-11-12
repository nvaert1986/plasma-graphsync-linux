// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "backend.h"
#include "config.h"
#include "driveitem.h"
#include "graphsync_debug.h"
#include "migration.h"
#include "placeholder.h"

#include <KConfigGroup>
#include <KFormat>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDesktopServices>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

namespace GraphSync
{

namespace
{
const QString service = QStringLiteral("io.github.nvaert1986.GraphSync");
const QString objectPath = QStringLiteral("/GraphSync");

QDBusInterface *daemon(QObject *parent)
{
    // Calling a method starts graphsyncd through D-Bus activation when it is not running
    return new QDBusInterface(service, objectPath, service, QDBusConnection::sessionBus(), parent);
}
}

namespace
{
// The account from before 0.8.0 moves to the shared account file first, unless an install of
// plasma-onedrive-linux still waits to be taken over (then it moves after that import)
AuthConfig startAuth()
{
    if (!Migration::pending()) {
        Migration::adoptSharedAccounts(0);
    }
    return Config::currentAuth();
}
}

Backend::Backend(QObject *parent)
    : QObject(parent)
    , m_auth(std::make_unique<Auth>(startAuth()))
    , m_graph(std::make_unique<GraphClient>(m_auth.get()))
    , m_itemService(std::make_unique<ItemService>(m_graph.get()))
    , m_baseFolder(Config::general().baseFolder)
{
    m_itemService->setGraphProvider([this](const QString &accountId) {
        return graphForAccount(accountId);
    });
    removeVersionCopies(24 * 60 * 60);
    // An install of plasma-onedrive-linux waiting to be taken over: in the background (stopping
    // the old daemon can take a while), with a page that says so instead of the setup wizard
    if (Migration::pending()) {
        startImport();
    }

    // Signed in by graphsyncctl or an older version: the organisation name is still missing
    if (signedIn() && Config::account().organization.isEmpty()) {
        fetchOrganization();
    }

    auto *watcher = new QDBusServiceWatcher(service, QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &Backend::refreshStatus);
    QDBusConnection::sessionBus().connect(service, objectPath, service, QStringLiteral("StatusChanged"), this, SLOT(onStatusChanged(QString)));
    refreshStatus();
}

QString Backend::callDaemon(const QString &method, const QVariantList &arguments)
{
    QDBusMessage message = QDBusMessage::createMethodCall(service, objectPath, service, method);
    message.setArguments(arguments);
    const QDBusMessage reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 10000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return {};
    }
    return reply.arguments().constFirst().toString();
}

void Backend::refreshStatus()
{
    // Only ask a running graphsyncd; a call would otherwise start it through D-Bus activation
    m_daemonRunning = QDBusConnection::sessionBus().interface()->isServiceRegistered(service);
    if (m_daemonRunning) {
        onStatusChanged(callDaemon(QStringLiteral("Status")));
    } else {
        m_status.clear();
        Q_EMIT statusChanged();
    }
}

void Backend::onStatusChanged(const QString &json)
{
    m_daemonRunning = true;
    const QVariantMap previous = m_status;
    m_status = QJsonDocument::fromJson(json.toUtf8()).toVariant().toMap();
    Q_EMIT statusChanged();
    // Another app or the tray changed the current account, or a sign-in was lost or made
    const QString account = m_status.value(QStringLiteral("account")).toMap().value(QStringLiteral("id")).toString();
    if (account != previous.value(QStringLiteral("account")).toMap().value(QStringLiteral("id")).toString()
        || m_status.value(QStringLiteral("signedIn")) != previous.value(QStringLiteral("signedIn"))) {
        // Not while a sign-in runs or a new account is being added: m_auth is busy with that one
        if (!busy() && m_loginHint.isEmpty() && Config::account().objectId != m_auth->config().objectId) {
            m_auth->setConfig(Config::currentAuth());
        }
        resetAccountClients();
        Q_EMIT accountChanged();
    }
}

QVariantMap Backend::status() const
{
    return m_status;
}

bool Backend::daemonRunning() const
{
    return m_daemonRunning;
}

bool Backend::pauseOnMeteredNetwork() const
{
    return Config::general().pauseOnMeteredNetwork;
}

void Backend::setPauseOnMeteredNetwork(bool on)
{
    GeneralConfig general = Config::general();
    if (general.pauseOnMeteredNetwork == on) {
        return;
    }
    general.pauseOnMeteredNetwork = on;
    Config::setGeneral(general);
    callDaemon(QStringLiteral("Command"), {QStringLiteral("{\"command\":\"reloadSettings\"}")});
    Q_EMIT statusChanged();
}

QString Backend::version() const
{
    return QStringLiteral(GRAPHSYNC_VERSION);
}

QString Backend::stateFolder() const
{
    return Config::stateDirectory();
}

QString Backend::defaultLinkType() const
{
    const QString type = KSharedConfig::openConfig(QStringLiteral("graphsyncrc"))->group(QStringLiteral("General")).readEntry("DefaultLinkType", QStringLiteral("view"));
    return type == QLatin1String("edit") ? type : QStringLiteral("view");
}

void Backend::setDefaultLinkType(const QString &type)
{
    KConfigGroup group = KSharedConfig::openConfig(QStringLiteral("graphsyncrc"))->group(QStringLiteral("General"));
    group.writeEntry("DefaultLinkType", type == QLatin1String("edit") ? type : QStringLiteral("view"));
    group.sync();
    Q_EMIT defaultLinkTypeChanged();
}

void Backend::updateGeneral(const std::function<void(GeneralConfig &)> &change)
{
    GeneralConfig general = Config::general();
    change(general);
    Config::setGeneral(general);
    // Only a running daemon needs to know; it reads the rest at its next cycle
    if (m_daemonRunning) {
        sendCommand({{QStringLiteral("command"), QStringLiteral("reloadSettings")}});
    }
    Q_EMIT advancedChanged();
}

int Backend::downloadLimit() const
{
    return Config::general().downloadLimitKBps;
}

void Backend::setDownloadLimit(int kbps)
{
    updateGeneral([kbps](GeneralConfig &general) {
        general.downloadLimitKBps = std::max(0, kbps);
    });
}

int Backend::uploadLimit() const
{
    return Config::general().uploadLimitKBps;
}

void Backend::setUploadLimit(int kbps)
{
    updateGeneral([kbps](GeneralConfig &general) {
        general.uploadLimitKBps = std::max(0, kbps);
    });
}

int Backend::minimumFreeSpace() const
{
    return Config::general().minimumFreeSpaceMB;
}

void Backend::setMinimumFreeSpace(int megabytes)
{
    updateGeneral([megabytes](GeneralConfig &general) {
        general.minimumFreeSpaceMB = std::max(0, megabytes);
    });
}

QString Backend::excludePatterns() const
{
    return Config::general().excludePatterns.join(QLatin1Char('\n'));
}

void Backend::setExcludePatterns(const QString &patterns)
{
    // One per line or separated by commas
    QStringList list;
    const QStringList parts = QString(patterns).replace(QLatin1Char(','), QLatin1Char('\n')).split(QLatin1Char('\n'));
    for (const QString &part : parts) {
        if (!part.trimmed().isEmpty() && !list.contains(part.trimmed())) {
            list.append(part.trimmed());
        }
    }
    if (list == Config::general().excludePatterns) {
        return;
    }
    updateGeneral([list](GeneralConfig &general) {
        general.excludePatterns = list;
    });
}

bool Backend::skipDotFiles() const
{
    return Config::general().skipDotFiles;
}

void Backend::setSkipDotFiles(bool skip)
{
    updateGeneral([skip](GeneralConfig &general) {
        general.skipDotFiles = skip;
    });
}

void Backend::recheckAll()
{
    sendCommand({{QStringLiteral("command"), QStringLiteral("recheck")}});
}

GraphClient *Backend::graphForAccount(const QString &accountId)
{
    if (accountId.isEmpty()) {
        return m_graph.get();
    }
    const AccountInfo account = GraphCore::Accounts::find(accountId);
    if (!account.isValid()) {
        // Gone: never another account's sign-in instead
        return nullptr;
    }
    // The current account: the app's own sign-in (unless that one is busy adding a new account)
    if (account.objectId == m_auth->config().objectId && account.tenantId == m_auth->config().tenantId) {
        return m_graph.get();
    }
    auto it = m_accountClients.find(accountId);
    if (it == m_accountClients.end()) {
        AccountClient client;
        client.auth = std::make_unique<Auth>(Config::authFor(account));
        client.graph = std::make_unique<GraphClient>(client.auth.get());
        it = m_accountClients.emplace(accountId, std::move(client)).first;
    }
    return it->second.graph.get();
}

void Backend::resetAccountClients()
{
    // Kept, not deleted: a status update can arrive while one of them waits for Graph. The
    // sign-in is read again on its next use
    for (auto &[accountId, client] : m_accountClients) {
        client.auth->setConfig(Config::authFor(GraphCore::Accounts::find(accountId)));
    }
}

QString Backend::accountOfPath(const QString &path)
{
    return Config::rootForPath(path).accountId;
}

ItemService::Item Backend::item(const QString &path)
{
    const QString key = QDir::cleanPath(path);
    const auto cached = m_items.constFind(key);
    if (cached != m_items.constEnd()) {
        return *cached;
    }
    setBusy(true);
    QString error;
    const ItemService::Item found = m_itemService->resolve(key, &error);
    setBusy(false);
    setError(error);
    if (found.valid()) {
        m_items.insert(key, found);
    }
    return found;
}

bool Backend::sendCommand(const QJsonObject &command)
{
    // Starts graphsyncd through D-Bus activation when it is not running
    const QString reply = callDaemon(QStringLiteral("Command"), {QString::fromUtf8(QJsonDocument(command).toJson(QJsonDocument::Compact))});
    return QJsonDocument::fromJson(reply.toUtf8()).object().value(QLatin1String("ok")).toBool();
}

QVariantMap Backend::itemInfo(const QString &path)
{
    const ItemService::Item found = item(path);
    return {
        {QStringLiteral("ok"), found.valid()},
        // The local name: a library root is called "root" online
        {QStringLiteral("name"), QFileInfo(path).fileName()},
        {QStringLiteral("isFolder"), found.valid() ? found.isFolder : QFileInfo(path).isDir()},
        {QStringLiteral("webUrl"), found.webUrl},
        {QStringLiteral("permissionsUrl"), found.permissionsUrl},
        // "People in <organisation>": the organisation of the file's own account, not the current one
        {QStringLiteral("organization"), GraphCore::Accounts::find(found.valid() ? found.accountId : accountOfPath(path)).organization},
    };
}

QVariantMap Backend::createLink(const QString &path, const QString &type, const QString &scope, int expiryDays)
{
    const ItemService::Item found = item(path);
    if (!found.valid()) {
        return {};
    }
    setBusy(true);
    QString error;
    const QDateTime expiry = expiryDays > 0 ? QDateTime::currentDateTimeUtc().addDays(expiryDays) : QDateTime();
    QVariantMap link = m_itemService->createLink(found, type, scope, expiry, &error);
    setBusy(false);
    setError(error);
    link.insert(QStringLiteral("ok"), error.isEmpty());
    return link;
}

bool Backend::copyText(const QString &text)
{
    if (!sendCommand({{QStringLiteral("command"), QStringLiteral("copyText")}, {QStringLiteral("text"), text}})) {
        setError(i18n("Could not put the link on the clipboard."));
        return false;
    }
    return true;
}

QVariantList Backend::permissions(const QString &path)
{
    const ItemService::Item found = item(path);
    if (!found.valid()) {
        return {};
    }
    setBusy(true);
    QString error;
    const QVariantList list = m_itemService->permissions(found, &error);
    setBusy(false);
    setError(error);
    return list;
}

bool Backend::removePermission(const QString &path, const QString &permissionId)
{
    const ItemService::Item found = item(path);
    if (!found.valid()) {
        return false;
    }
    setBusy(true);
    QString error;
    const bool removed = m_itemService->removePermission(found, permissionId, &error);
    setBusy(false);
    setError(error);
    return removed;
}

bool Backend::invite(const QString &path, const QStringList &emails, const QString &role, bool notify, const QString &message)
{
    const ItemService::Item found = item(path);
    if (!found.valid()) {
        return false;
    }
    setBusy(true);
    QString error;
    const bool invited = m_itemService->invite(found, emails, role, notify, message, &error);
    setBusy(false);
    setError(error);
    return invited;
}

QVariantMap Backend::searchPeople(const QString &path, const QString &query)
{
    // No busy indicator: this runs while the user types
    bool notAllowed = false;
    QString error;
    const QVariantList people = m_itemService->searchPeople(query, accountOfPath(path), &notAllowed, &error);
    return {{QStringLiteral("people"), people}, {QStringLiteral("notAllowed"), notAllowed}};
}

QVariantList Backend::versions(const QString &path)
{
    const ItemService::Item found = item(path);
    if (!found.valid()) {
        return {};
    }
    setBusy(true);
    QString error;
    QVariantList list = m_itemService->versions(found, &error);
    setBusy(false);
    setError(error);
    const KFormat format;
    for (QVariant &value : list) {
        QVariantMap version = value.toMap();
        version.insert(QStringLiteral("sizeText"), format.formatByteSize(version.value(QStringLiteral("size")).toDouble()));
        value = version;
    }
    return list;
}

bool Backend::openVersion(const QString &path, const QString &versionId)
{
    const ItemService::Item found = item(path);
    if (!found.valid()) {
        return false;
    }
    // "Report (version 3.0).docx", read-only, so nobody edits an old copy by accident
    const QFileInfo info(path);
    const QString suffix = info.completeSuffix().isEmpty() ? QString() : QLatin1Char('.') + info.completeSuffix();
    if (!DriveItem::isSafeId(found.id)) {
        return false;
    }
    const QString folder = versionCacheFolder() + QLatin1Char('/') + found.id;
    QDir().mkpath(folder);
    const QString target = folder + QLatin1Char('/') + i18nc("file name of an old version: name, version, extension", "%1 (version %2)%3", info.baseName(), versionId, suffix);
    QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner);

    setBusy(true);
    QString error;
    const bool downloaded = m_itemService->downloadVersion(found, versionId, target, &error);
    setBusy(false);
    setError(error);
    if (!downloaded) {
        return false;
    }
    QFile::setPermissions(target, QFile::ReadOwner);
    QDesktopServices::openUrl(QUrl::fromLocalFile(target));
    return true;
}

bool Backend::saveVersionAs(const QString &path, const QString &versionId, const QUrl &target)
{
    const ItemService::Item found = item(path);
    if (!found.valid() || !target.isLocalFile()) {
        return false;
    }
    setBusy(true);
    QString error;
    const bool saved = m_itemService->downloadVersion(found, versionId, target.toLocalFile(), &error);
    setBusy(false);
    setError(error);
    return saved;
}

bool Backend::restoreVersion(const QString &path, const QString &versionId)
{
    const ItemService::Item found = item(path);
    if (!found.valid()) {
        return false;
    }
    setBusy(true);
    QString error;
    const bool restored = m_itemService->restoreVersion(found, versionId, &error);
    setBusy(false);
    setError(error);
    if (restored) {
        // Bring the restored version down now instead of with the next poll
        syncNow();
    }
    return restored;
}

QString Backend::openedBy(const QString &path) const
{
    const QFileInfo info(path);
    // LibreOffice keeps a lock file next to the document while it is open
    if (QFileInfo::exists(info.dir().filePath(QStringLiteral(".~lock.") + info.fileName() + QLatin1Char('#')))) {
        return QStringLiteral("LibreOffice");
    }
    // Other programs that keep the file open (only our own processes are readable)
    const QString target = info.canonicalFilePath();
    QDirIterator processes(QStringLiteral("/proc"), QDir::Dirs | QDir::NoDotAndDotDot);
    while (processes.hasNext()) {
        const QString pid = processes.next();
        QDirIterator fds(pid + QStringLiteral("/fd"), QDir::Files | QDir::System);
        while (fds.hasNext()) {
            if (QFileInfo(fds.next()).symLinkTarget() == target) {
                QFile comm(pid + QStringLiteral("/comm"));
                return comm.open(QIODevice::ReadOnly) ? QString::fromUtf8(comm.readAll()).trimmed() : QString();
            }
        }
    }
    return {};
}

int Backend::viewOnline(const QString &path)
{
    const QString cleanPath = QDir::cleanPath(path);
    const bool isFile = QFileInfo(cleanPath).isFile();
    QString error;
    const ItemService::Item found = m_itemService->resolve(cleanPath, &error);
    QUrl url;
    if (found.valid() && !found.webUrl.isEmpty()) {
        // Office files come as Doc.aspx (the browser editor); other files would download, so ask
        // SharePoint for its preview instead
        url = QUrl(found.webUrl);
        if (isFile && !found.webUrl.contains(QLatin1String("/_layouts/"))) {
            url.setQuery(QStringLiteral("web=1"));
        }
    } else {
        // Not online yet, or no network: the address built from the library and the path
        const QList<RootConfig> roots = Config::roots();
        for (const RootConfig &root : roots) {
            const QString rootPath = QDir::cleanPath(root.localPath);
            if (root.webUrl.isEmpty() || !(cleanPath == rootPath || cleanPath.startsWith(rootPath + QLatin1Char('/')))) {
                continue;
            }
            QByteArray encoded = root.webUrl.toUtf8();
            const QStringList parts = cleanPath.mid(rootPath.size() + 1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                encoded += '/' + QUrl::toPercentEncoding(part);
            }
            if (isFile) {
                encoded += "?web=1";
            }
            url = QUrl::fromEncoded(encoded);
            break;
        }
    }
    if (!url.isValid() || url.isEmpty()) {
        return 1;
    }
    return QDesktopServices::openUrl(url) ? 0 : 1;
}

int Backend::openInternetShortcut(const QString &pathOrUrl)
{
    const QString path = pathOrUrl.startsWith(QLatin1String("file:")) ? QUrl(pathOrUrl).toLocalFile() : pathOrUrl;
    QFile file(path);
    // A real .url file is a few hundred bytes
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
        qWarning().noquote() << "Cannot read" << path;
        return 1;
    }
    // [InternetShortcut]
    // URL=https://...
    bool inSection = false;
    QUrl url;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inSection = line.compare(QLatin1String("[InternetShortcut]"), Qt::CaseInsensitive) == 0;
        } else if (inSection && line.startsWith(QLatin1String("URL="), Qt::CaseInsensitive)) {
            url = QUrl(line.mid(4), QUrl::StrictMode);
            break;
        }
    }
    // Shortcut files can also point to local programs or scripts; never follow those
    if (!url.isValid() || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) {
        qWarning().noquote() << "No web address in" << path;
        return 1;
    }
    return QDesktopServices::openUrl(url) ? 0 : 1;
}

int Backend::openPlaceholder(const QString &pathOrUrl)
{
    const QString path = QDir::cleanPath(pathOrUrl.startsWith(QLatin1String("file:")) ? QUrl(pathOrUrl).toLocalFile() : pathOrUrl);
    const QString real = Placeholder::realPathOf(path);
    if (!Placeholder::isPlaceholder(path)) {
        // Downloaded meanwhile (opened twice), or a .cloud file of something else
        if (QFileInfo::exists(real) && real != path) {
            return QDesktopServices::openUrl(QUrl::fromLocalFile(real)) ? 0 : 1;
        }
        qWarning().noquote() << path << "is not an online-only placeholder";
        return 1;
    }
    if (!hydrateAndWait(path)) {
        return 1;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(real)) ? 0 : 1;
}

bool Backend::hydrateAndWait(const QString &path)
{
    const QString real = Placeholder::realPathOf(path);
    if (!sendCommand({{QStringLiteral("command"), QStringLiteral("hydrate")}, {QStringLiteral("path"), path}})) {
        qWarning().noquote() << path << "is not in a synced folder, or the sync service is not running";
        return false;
    }
    // A large file takes a while; the tray shows the progress and the daemon reports a failure
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 60 * 60 * 1000) {
        QEventLoop pause;
        QTimer::singleShot(300, &pause, &QEventLoop::quit);
        pause.exec();
        const QString reply = callDaemon(QStringLiteral("Command"),
                                         {QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("command"), QStringLiteral("hydrateState")},
                                                                                      {QStringLiteral("path"), path}})
                                                                .toJson(QJsonDocument::Compact))});
        const QString state = QJsonDocument::fromJson(reply.toUtf8()).object().value(QLatin1String("state")).toString();
        if (state == QLatin1String("done") || (state.isEmpty() && QFileInfo::exists(real))) {
            return true;
        }
        if (state.startsWith(QLatin1String("failed"))) {
            return false;
        }
    }
    return false;
}

int Backend::downloadPlaceholders(const QStringList &pathsOrUrls)
{
    int failed = 0;
    for (const QString &pathOrUrl : pathsOrUrls) {
        const QString path = QDir::cleanPath(pathOrUrl.startsWith(QLatin1String("file:")) ? QUrl(pathOrUrl).toLocalFile() : pathOrUrl);
        // Already downloaded (picked twice), or not one of ours: nothing to do
        if (!Placeholder::isPlaceholder(path)) {
            continue;
        }
        if (!hydrateAndWait(path)) {
            ++failed;
        }
    }
    return failed == 0 ? 0 : 1;
}

void Backend::setFilesOnDemand(const QString &rootId, bool on)
{
    Config::setFilesOnDemand(rootId, on);
    // The engine reads it at the start of its next cycle; switched off, it downloads everything
    syncNow();
    refreshStatus();
}

int Backend::copyLink(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    auto fail = [this, &name](const QString &error) {
        sendCommand({{QStringLiteral("command"), QStringLiteral("linkFailed")}, {QStringLiteral("name"), name}, {QStringLiteral("error"), error}});
        return 1;
    };
    QString error;
    const ItemService::Item found = m_itemService->resolve(path, &error);
    if (!found.valid()) {
        return fail(error);
    }
    // No scope: the organisation's default link, like OneDrive's Copy link
    const QVariantMap link = m_itemService->createLink(found, defaultLinkType(), {}, {}, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }
    const bool copied = sendCommand({{QStringLiteral("command"), QStringLiteral("linkCopied")},
                                     {QStringLiteral("url"), link.value(QStringLiteral("url")).toString()},
                                     {QStringLiteral("path"), QDir::cleanPath(path)},
                                     {QStringLiteral("name"), name},
                                     {QStringLiteral("scope"), link.value(QStringLiteral("scope")).toString()},
                                     {QStringLiteral("type"), link.value(QStringLiteral("type")).toString()}});
    return copied ? 0 : 1;
}

void Backend::pause(int minutes)
{
    callDaemon(QStringLiteral("Pause"), {minutes});
}

void Backend::resume()
{
    callDaemon(QStringLiteral("Resume"));
}

void Backend::syncNow()
{
    callDaemon(QStringLiteral("SyncNow"));
}

bool Backend::removeRoot(const QString &id)
{
    QDBusMessage message = QDBusMessage::createMethodCall(service, objectPath, service, QStringLiteral("RemoveRoot"));
    message << id;
    const QDBusMessage reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 10000);
    return reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty() && reply.arguments().constFirst().toBool();
}

QUrl Backend::fileUrl(const QString &path)
{
    return QUrl::fromLocalFile(path);
}

QString Backend::localPath(const QUrl &url)
{
    return url.toLocalFile();
}

Backend::~Backend()
{
    // The import must not be cut off halfway; it is bounded (lock wait, old daemon stop)
    if (m_importThread) {
        m_importThread->wait();
    }
}

QString Backend::importState() const
{
    return m_importState;
}

QString Backend::importMessage() const
{
    return m_importMessage;
}

void Backend::retryImport()
{
    if (m_importState != QLatin1String("running")) {
        startImport();
    }
}

void Backend::startImport()
{
    m_importState = QStringLiteral("running");
    m_importMessage.clear();
    Q_EMIT importStateChanged();
    m_importThread = QThread::create([this] {
        QString message;
        const Migration::Result result = Migration::importLegacyInstall(&message);
        QMetaObject::invokeMethod(
            this,
            [this, result, message] {
                importDone(static_cast<int>(result), message);
            },
            Qt::QueuedConnection);
    });
    connect(m_importThread, &QThread::finished, m_importThread, &QObject::deleteLater);
    m_importThread->start();
}

void Backend::importDone(int result, const QString &message)
{
    switch (static_cast<Migration::Result>(result)) {
    case Migration::Result::NotNeeded:
    case Migration::Result::Imported:
        m_importState.clear();
        break;
    case Migration::Result::Busy:
        m_importState = QStringLiteral("failed");
        m_importMessage = i18n("The previous version (plasma-onedrive-linux) is still busy; try again in a moment.");
        break;
    case Migration::Result::Failed:
        m_importState = QStringLiteral("failed");
        m_importMessage = i18n("Your settings from plasma-onedrive-linux could not be taken over: %1", message);
        break;
    }
    // Everything read at start may have changed: registration, account, folders, setup
    Migration::adoptSharedAccounts(0);
    m_auth->setConfig(Config::currentAuth());
    m_baseFolder = Config::general().baseFolder;
    if (static_cast<Migration::Result>(result) == Migration::Result::Imported && setupDone()) {
        startDaemon();
    }
    callDaemon(QStringLiteral("ReloadAccount"));
    Q_EMIT configChanged();
    Q_EMIT accountChanged();
    Q_EMIT baseFolderChanged();
    Q_EMIT importStateChanged();
    Q_EMIT importFinished();
}

QString Backend::versionCacheFolder()
{
    // Copies of old versions: owner only, removed after a day and at sign-out
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/graphsync");
    const QString folder = base + QStringLiteral("/versions");
    QDir().mkpath(folder);
    QFile::setPermissions(base, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return folder;
}

void Backend::removeVersionCopies(qint64 olderThanSeconds)
{
    const QDateTime limit = QDateTime::currentDateTime().addSecs(-olderThanSeconds);
    QDirIterator it(versionCacheFolder(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        if (it.fileInfo().lastModified() <= limit) {
            QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner);
            QFile::remove(file);
        }
    }
}

void Backend::signOut()
{
    signOutAccount(Config::account().id);
}

void Backend::accountCommand(const QString &name, const QString &id)
{
    const QJsonObject command{{QStringLiteral("command"), name}, {QStringLiteral("id"), id}};
    callDaemon(QStringLiteral("Command"), {QString::fromUtf8(QJsonDocument(command).toJson(QJsonDocument::Compact))});
    // A sign-out may have handed the current account to another one; the other accounts' sign-ins
    // are read again when needed
    m_auth->setConfig(Config::currentAuth());
    resetAccountClients();
    m_items.clear();
    refreshStatus();
    Q_EMIT accountChanged();
    Q_EMIT configChanged();
}

bool Backend::setCurrentAccount(const QString &id)
{
    if (!GraphCore::Accounts::find(id).isValid()) {
        return false;
    }
    if (id == Config::account().id) {
        return true;
    }
    // Also without a running daemon (the next start reads it)
    Config::setCurrentAccountId(id);
    accountCommand(QStringLiteral("setCurrentAccount"), id);
    return true;
}

void Backend::signOutAccount(const QString &id)
{
    if (id.isEmpty()) {
        return;
    }
    if (id == Config::account().id) {
        // Copies of old versions came from this account's files
        removeVersionCopies(0);
    }
    if (m_daemonRunning) {
        accountCommand(QStringLiteral("signOutAccount"), id);
        return;
    }
    // No daemon: sign out here
    const AccountInfo account = GraphCore::Accounts::find(id);
    if (account.isValid()) {
        Auth auth(Config::authFor(account));
        auth.logout();
    }
    m_auth->setConfig(Config::currentAuth());
    // Its tokens may still be in memory from Share or Version History
    resetAccountClients();
    Q_EMIT accountChanged();
}

void Backend::removeAccount(const QString &id)
{
    if (id.isEmpty() || !m_daemonRunning) {
        // Removing locations is the daemon's work (Places entries, its state)
        return;
    }
    if (id == Config::account().id) {
        removeVersionCopies(0);
    }
    accountCommand(QStringLiteral("removeAccount"), id);
}

bool Backend::trayNeedsPanelRestart() const
{
    static const QString appletId = QStringLiteral("io.github.nvaert1986.graphsync.tray");
    if (QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("plasma/plasmoids/%1/metadata.json").arg(appletId)).isEmpty()) {
        return false;
    }
    // Only with a Plasma panel in this session (not on other desktops, nor another session of
    // the same user)
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()->isServiceRegistered(QStringLiteral("org.kde.plasmashell"))) {
        return false;
    }
    // The system tray lists every applet it has seen in knownItems; ours missing there means the
    // panel started before the package was installed and does not know the applet yet
    // (the tray is an applet inside a panel: [Containments][2][Applets][7][General], so search the
    // whole tree)
    const KSharedConfigPtr panel = KSharedConfig::openConfig(QStringLiteral("plasma-org.kde.plasma.desktop-appletsrc"), KConfig::SimpleConfig);
    panel->reparseConfiguration();
    // Without a system tray in any panel there is no icon to bring back by restarting
    bool hasTray = false;
    bool known = false;
    const std::function<void(const KConfigGroup &)> visit = [&](const KConfigGroup &group) {
        hasTray = hasTray || group.readEntry("plugin", QString()) == QLatin1String("org.kde.plasma.systemtray");
        known = known || group.readEntry("knownItems", QStringList()).contains(appletId);
        const QStringList children = group.groupList();
        for (const QString &child : children) {
            visit(group.group(child));
        }
    };
    visit(panel->group(QStringLiteral("Containments")));
    return hasTray && !known;
}

void Backend::restartPanel()
{
    // Plasma started by systemd (the default) or the classic way. "restart" on an inactive unit
    // would start a second panel that exits as a duplicate, so check which one runs
    QProcess::startDetached(QStringLiteral("sh"),
                            {QStringLiteral("-c"),
                             QStringLiteral("if systemctl --user --quiet is-active plasma-plasmashell.service; then "
                                            "systemctl --user restart plasma-plasmashell.service; "
                                            "else kquitapp6 plasmashell; kstart plasmashell; fi")});
}

void Backend::startDaemon()
{
    QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("start"), QStringLiteral("plasma-graphsync-linux.service")});
}

QString Backend::addPersonalFor(const QString &accountId)
{
    // Another account's OneDrive (an account card in Settings): with that account's sign-in, its
    // organisation and folder name, without making it the current account
    const AccountInfo account = GraphCore::Accounts::find(accountId);
    GraphClient *graph = graphForAccount(accountId);
    if (!account.isValid() || !graph) {
        return {};
    }
    setBusy(true);
    const GraphResponse drive = graph->get(QStringLiteral("/me/drive?$select=id,webUrl"));
    setBusy(false);
    if (!drive.ok()) {
        setError(drive.describe());
        return {};
    }
    const QString label = account.organization.isEmpty() ? organization() : Config::organizationLabel(account);
    return addRoot({{QStringLiteral("name"), i18nc("name of the personal OneDrive, %1 is the organization", "OneDrive - %1", account.organization)},
                    {QStringLiteral("localPath"), QDir(m_baseFolder).filePath(i18nc("folder name of the personal OneDrive, %1 is the organization", "OneDrive - %1", safeName(label)))},
                    {QStringLiteral("driveId"), drive.json().value(QLatin1String("id")).toString()},
                    {QStringLiteral("kind"), QStringLiteral("personal")},
                    {QStringLiteral("webUrl"), drive.json().value(QLatin1String("webUrl")).toString()},
                    {QStringLiteral("accountId"), account.id}});
}

QString Backend::addPersonal()
{
    const QVariantMap drive = personalDrive();
    if (drive.value(QStringLiteral("id")).toString().isEmpty()) {
        return {};
    }
    return addRoot({{QStringLiteral("name"), i18nc("name of the personal OneDrive, %1 is the organization", "OneDrive - %1", organization())},
                    {QStringLiteral("localPath"), personalPath()},
                    {QStringLiteral("driveId"), drive.value(QStringLiteral("id"))},
                    {QStringLiteral("kind"), QStringLiteral("personal")},
                    {QStringLiteral("webUrl"), drive.value(QStringLiteral("webUrl"))}});
}

void Backend::openNotificationSettings()
{
    QProcess::startDetached(QStringLiteral("systemsettings"), {QStringLiteral("kcm_notifications"), QStringLiteral("--args"), QStringLiteral("--notifyrc plasma-graphsync-linux")});
}

void Backend::showInFolder(const QString &path)
{
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                          QStringLiteral("/org/freedesktop/FileManager1"),
                                                          QStringLiteral("org.freedesktop.FileManager1"),
                                                          QStringLiteral("ShowFolders"));
    message << QStringList{QUrl::fromLocalFile(path).toString()} << QString();
    QDBusConnection::sessionBus().call(message, QDBus::NoBlock);
}

void Backend::launchApp(const QStringList &arguments)
{
    QProcess::startDetached(QStringLiteral(GRAPHSYNC_BINDIR "/plasma-graphsync-linux"), arguments);
}

bool Backend::signedIn() const
{
    if (!Config::account().isValid() || !m_auth->isConfigured()) {
        return false;
    }
    // The current account may be one that is signed out (made current to sign it in again): the
    // daemon knows whether it has a working sign-in
    return !m_daemonRunning || m_status.value(QStringLiteral("signedIn")).toBool();
}

QString Backend::displayName() const
{
    return Config::account().displayName;
}

QString Backend::userPrincipalName() const
{
    return Config::account().userPrincipalName;
}

QString Backend::organization() const
{
    const QString org = Config::account().organization;
    return org.isEmpty() ? i18n("Organization") : org;
}

QString Backend::baseFolder() const
{
    return m_baseFolder;
}

void Backend::setBaseFolder(const QString &folder)
{
    QString path = folder;
    if (path.startsWith(QLatin1String("file://"))) {
        path = QUrl(path).toLocalFile();
    }
    if (path == m_baseFolder) {
        return;
    }
    m_baseFolder = QDir::cleanPath(path);
    GeneralConfig general = Config::general();
    general.baseFolder = m_baseFolder;
    Config::setGeneral(general);
    Q_EMIT baseFolderChanged();
}

bool Backend::setupDone() const
{
    return Config::general().setupDone;
}

QString Backend::lastError() const
{
    return m_lastError;
}

bool Backend::busy() const
{
    return m_busy;
}

void Backend::setError(const QString &error)
{
    // Not approved by the organisation yet: offer the page where its administrator does that
    const AuthConfig config = m_auth->config();
    const QString consent = Auth::needsAdminConsent(error) ? Auth::adminConsentUrl(config.tenantId, config.clientId).toString() : QString();
    if (m_lastError != error || m_adminConsentLink != consent) {
        m_lastError = error;
        m_adminConsentLink = consent;
        Q_EMIT lastErrorChanged();
    }
}

bool Backend::startNewAccount(const QString &email, const QString &clientId)
{
    const QString address = email.trimmed();
    const QString client = clientId.trimmed().isEmpty() ? defaultClientId() : clientId.trimmed().toLower();
    if (!Auth::isGuid(client)) {
        setError(i18n("The client ID looks like 00000000-0000-0000-0000-000000000000. Ask your IT administrator for it."));
        return false;
    }
    if (!address.contains(QLatin1Char('@'))) {
        setError(i18n("Enter the work e-mail address of the account."));
        return false;
    }
    setBusy(true);
    bool offline = false;
    const QString tenant = Auth::resolveTenant(address, &offline);
    setBusy(false);
    if (tenant.isEmpty()) {
        setError(offline ? i18n("No connection to Microsoft 365. Check the network and try again.")
                         : i18n("No Microsoft 365 organization was found for %1.", address));
        return false;
    }
    m_auth->setConfig({client, tenant, {}});
    m_loginHint = address;
    setError({});
    return true;
}

void Backend::cancelNewAccount()
{
    if (busy()) {
        // A sign-in still waits (in its own event loop) on m_auth: stop a code sign-in and switch
        // back once it returned; changing m_auth under it would store its token wrongly
        m_cancelSignIn = true;
        m_restoreAfterSignIn = true;
        return;
    }
    m_restoreAfterSignIn = false;
    m_loginHint.clear();
    m_auth->setConfig(Config::currentAuth());
    setError({});
}

bool Backend::endSignIn(bool ok, const QString &error)
{
    if (m_restoreAfterSignIn) {
        // The Add Account page was closed meanwhile. A sign-in that completed anyway registered
        // its account properly (and made it current); either way back to the current account
        m_restoreAfterSignIn = false;
        m_loginHint.clear();
        m_auth->setConfig(Config::currentAuth());
        if (ok) {
            return finishSignIn();
        }
        return false;
    }
    if (!ok) {
        if (!m_cancelSignIn) {
            setError(error);
        }
        return false;
    }
    return finishSignIn();
}

QString Backend::defaultClientId() const
{
    const QString suite = GraphCore::Accounts::defaultClientId();
    return suite.isEmpty() ? Config::auth().clientId : suite;
}

void Backend::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        Q_EMIT busyChanged();
    }
}

GraphResponse Backend::get(const QString &path, const QString &accountId)
{
    GraphClient *graph = graphForAccount(accountId);
    if (!graph) {
        GraphResponse gone;
        gone.errorString = i18n("The account of this location is no longer on this computer. Sign in to it again in Settings > Accounts.");
        setError(gone.errorString);
        return gone;
    }
    setBusy(true);
    const GraphResponse response = graph->get(path);
    setBusy(false);
    setError(response.ok() ? QString() : response.describe());
    return response;
}

bool Backend::signIn()
{
    if (!m_auth->isConfigured()) {
        setError(i18n("The app registration is not configured (client and tenant ID)."));
        return false;
    }
    if constexpr (!Auth::browserLoginAvailable()) {
        // Builds on Qt older than 6.9: every Sign In button signs in with a code
        return signInWithCode();
    }
    m_cancelSignIn = false;
    setBusy(true);
    QString error;
    // Signing in again: Microsoft's account picker starts at the account that expired
    const bool ok = m_auth->interactiveLogin(&error, m_loginHint.isEmpty() ? Config::account().userPrincipalName : m_loginHint);
    setBusy(false);
    return endSignIn(ok, error);
}

bool Backend::configured() const
{
    return m_auth->isConfigured();
}

QString Backend::clientId() const
{
    return Config::auth().clientId;
}

QString Backend::tenantId() const
{
    return Config::auth().tenantId;
}

bool Backend::saveAppRegistration(const QString &client, const QString &tenant)
{
    AuthConfig config = Auth::parseSetupText(client);
    if (config.clientId.isEmpty()) {
        config.clientId = client.trimmed();
    }
    const QString tenantInput = tenant.trimmed().isEmpty() ? config.tenantId : tenant.trimmed();
    if (!Auth::isGuid(config.clientId)) {
        setError(i18n("The client ID looks like 00000000-0000-0000-0000-000000000000. Ask your IT administrator for it."));
        return false;
    }
    if (tenantInput.isEmpty()) {
        setError(i18n("Enter your work e-mail address."));
        return false;
    }
    setBusy(true);
    bool offline = false;
    config.tenantId = Auth::resolveTenant(tenantInput, &offline);
    setBusy(false);
    if (config.tenantId.isEmpty()) {
        setError(offline ? i18n("No connection to Microsoft 365. Check the network and try again.")
                         : i18n("No Microsoft 365 organization was found for %1.", tenantInput));
        return false;
    }
    config = Auth::normalized(config);

    // Stored values can be spelled differently (older versions kept case, or a domain as tenant):
    // the same registration in another spelling keeps the sign-in, moved to the new spelling's key
    const AuthConfig stored = Config::auth();
    AuthConfig old = Auth::normalized(stored);
    if (!old.tenantId.isEmpty() && !Auth::isGuid(old.tenantId)) {
        bool unused = false;
        const QString resolved = Auth::resolveTenant(old.tenantId, &unused);
        if (!resolved.isEmpty()) {
            old.tenantId = resolved;
        }
    }
    const bool sameRegistration = old.clientId == config.clientId && old.tenantId == config.tenantId;
    // Only an account of this registration's own organisation is affected; accounts of other
    // organisations keep their sign-in as it is
    const AuthConfig current = m_auth->config();
    // ... and an account that signs in with another registration (the suite's, or its own) is not
    // affected either: only tenant and client ID together
    const bool currentOfIt = !current.objectId.isEmpty() && Auth::normalized(current).tenantId == old.tenantId
        && Auth::normalized(current).clientId == old.clientId;
    bool reloadDaemon = false;
    if (sameRegistration && currentOfIt) {
        // The current account keeps its sign-in, under the key of the new spelling
        AuthConfig target = config;
        target.objectId = current.objectId;
        if (Auth::keychainKeyFor(current) != Auth::keychainKeyFor(target)) {
            QString moveError;
            if (!Auth::moveToken(current, target, &moveError)) {
                qCWarning(GRAPHSYNC) << "Could not move the sign-in to the new spelling:" << moveError;
            }
            reloadDaemon = true;
        }
        m_auth->setConfig(target);
    } else if (!sameRegistration && currentOfIt) {
        // The stored sign-in belongs to the old registration. signOut() alone: it signs this account
        // out (through the daemon) and then m_auth already belongs to the next current account
        signOut();
    }
    Config::setAuth(config);
    // Not signed in (any more): the next sign-in uses the new registration
    if (!Config::account().isValid()) {
        m_auth->setConfig(config);
    }
    if (reloadDaemon) {
        callDaemon(QStringLiteral("ReloadAccount"));
    }
    setError({});
    Q_EMIT configChanged();
    Q_EMIT accountChanged();
    return true;
}

bool Backend::finishSignIn()
{
    m_loginHint.clear();
    // The account that signed in may have been used through an older sign-in
    resetAccountClients();
    setError({});
    fetchOrganization();
    // Let a running graphsyncd pick up the new refresh token: only this account's locations
    // restart, the other accounts' transfers go on
    std::unique_ptr<QDBusInterface> iface(daemon(nullptr));
    const QJsonObject reload{{QStringLiteral("command"), QStringLiteral("reloadAccount")}, {QStringLiteral("id"), Config::account().id}};
    iface->call(QStringLiteral("Command"), QString::fromUtf8(QJsonDocument(reload).toJson(QJsonDocument::Compact)));
    Q_EMIT accountChanged();
    return true;
}

bool Backend::signInWithCode()
{
    if (!m_auth->isConfigured()) {
        setError(i18n("The app registration is not configured (client and tenant ID)."));
        return false;
    }
    m_cancelSignIn = false;
    setError({});
    setBusy(true);
    QString error;
    const bool ok = m_auth->deviceCodeLogin(
        [this](const QString &code, const QString &address) {
            m_deviceCode = code;
            m_deviceCodeAddress = address;
            Q_EMIT deviceCodeChanged();
        },
        &m_cancelSignIn,
        &error);
    m_deviceCode.clear();
    m_deviceCodeAddress.clear();
    Q_EMIT deviceCodeChanged();
    setBusy(false);
    return endSignIn(ok, error);
}

void Backend::cancelSignIn()
{
    m_cancelSignIn = true;
}

void Backend::fetchOrganization()
{
    const GraphResponse response = get(QStringLiteral("/organization?$select=displayName"));
    const QJsonArray values = response.json().value(QLatin1String("value")).toArray();
    if (values.isEmpty()) {
        return;
    }
    Config::setAccountOrganization(Config::account().id, values.at(0).toObject().value(QLatin1String("displayName")).toString());
}

QVariantList Backend::siteList(const QString &path)
{
    QVariantList sites;
    const GraphResponse response = get(path);
    const QJsonArray values = response.json().value(QLatin1String("value")).toArray();
    for (const QJsonValue &value : values) {
        const QJsonObject site = value.toObject();
        sites.append(QVariantMap{{QStringLiteral("id"), site.value(QLatin1String("id")).toString()},
                                 {QStringLiteral("name"), site.value(QLatin1String("displayName")).toString()},
                                 {QStringLiteral("webUrl"), site.value(QLatin1String("webUrl")).toString()}});
    }
    return sites;
}

QVariantList Backend::followedSites()
{
    return siteList(QStringLiteral("/me/followedSites?$select=id,displayName,webUrl"));
}

QVariantList Backend::searchSites(const QString &query)
{
    if (query.trimmed().isEmpty()) {
        return {};
    }
    return siteList(QStringLiteral("/sites?search=%1&$select=id,displayName,webUrl").arg(QString::fromLatin1(QUrl::toPercentEncoding(query))));
}

QVariantMap Backend::siteFromUrl(const QString &url)
{
    // Phase 0 T4: brand-new sites are missing from search, but always resolve by path
    const QUrl parsed(url.trimmed());
    if (!parsed.isValid() || parsed.host().isEmpty()) {
        setError(i18n("That is not a SharePoint address."));
        return {};
    }
    QString path = parsed.path();
    // Accept a link to a library or folder too: keep only /sites/<name> or /teams/<name>
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() >= 2 && (parts.at(0) == QLatin1String("sites") || parts.at(0) == QLatin1String("teams"))) {
        path = QLatin1Char('/') + parts.at(0) + QLatin1Char('/') + parts.at(1);
    }
    const GraphResponse response = get(QStringLiteral("/sites/%1:%2?$select=id,displayName,webUrl").arg(parsed.host(), path));
    if (!response.ok()) {
        return {};
    }
    const QJsonObject site = response.json();
    return {{QStringLiteral("id"), site.value(QLatin1String("id")).toString()},
            {QStringLiteral("name"), site.value(QLatin1String("displayName")).toString()},
            {QStringLiteral("webUrl"), site.value(QLatin1String("webUrl")).toString()}};
}

QVariantList Backend::libraries(const QString &siteId)
{
    QVariantList result;
    const GraphResponse response = get(QStringLiteral("/sites/%1/drives?$select=id,name,webUrl,driveType").arg(siteId));
    const QJsonArray values = response.json().value(QLatin1String("value")).toArray();
    for (const QJsonValue &value : values) {
        const QJsonObject drive = value.toObject();
        result.append(QVariantMap{{QStringLiteral("id"), drive.value(QLatin1String("id")).toString()},
                                  {QStringLiteral("name"), drive.value(QLatin1String("name")).toString()},
                                  {QStringLiteral("webUrl"), drive.value(QLatin1String("webUrl")).toString()}});
    }
    return result;
}

QVariantMap Backend::personalDrive()
{
    const GraphResponse response = get(QStringLiteral("/me/drive?$select=id,webUrl,quota"));
    if (!response.ok()) {
        return {};
    }
    const QJsonObject drive = response.json();
    return {{QStringLiteral("id"), drive.value(QLatin1String("id")).toString()},
            {QStringLiteral("webUrl"), drive.value(QLatin1String("webUrl")).toString()}};
}

QVariantList Backend::childFolders(const QString &driveId, const QString &itemId, const QString &rootId)
{
    QVariantList folders;
    const QString base = itemId.isEmpty() ? QStringLiteral("/drives/%1/root").arg(driveId) : QStringLiteral("/drives/%1/items/%2").arg(driveId, itemId);
    QString link = base + QStringLiteral("/children?$select=id,name,folder&$top=999");
    QString accountId;
    if (!rootId.isEmpty()) {
        const RootConfig root = Config::root(rootId);
        if (!GraphCore::Accounts::find(root.accountId).isValid()) {
            // Never list it with another account's sign-in
            setError(i18n("The account of %1 is no longer on this computer. Sign in to it again in Settings > Accounts.", root.name));
            return {};
        }
        accountId = root.accountId;
    }
    while (!link.isEmpty()) {
        const GraphResponse response = get(link, accountId);
        if (!response.ok()) {
            break;
        }
        const QJsonObject page = response.json();
        const QJsonArray values = page.value(QLatin1String("value")).toArray();
        for (const QJsonValue &value : values) {
            const QJsonObject item = value.toObject();
            if (!item.contains(QLatin1String("folder"))) {
                continue;
            }
            folders.append(QVariantMap{{QStringLiteral("id"), item.value(QLatin1String("id")).toString()},
                                       {QStringLiteral("name"), item.value(QLatin1String("name")).toString()},
                                       {QStringLiteral("childCount"), item.value(QLatin1String("folder")).toObject().value(QLatin1String("childCount")).toInt()}});
        }
        link = page.value(QLatin1String("@odata.nextLink")).toString();
    }
    std::sort(folders.begin(), folders.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("name")).toString().localeAwareCompare(b.toMap().value(QStringLiteral("name")).toString()) < 0;
    });
    return folders;
}

QVariantMap Backend::root(const QString &rootId)
{
    const RootConfig root = Config::root(rootId);
    if (root.id.isEmpty()) {
        return {};
    }
    return {{QStringLiteral("id"), root.id},
            {QStringLiteral("name"), root.name},
            {QStringLiteral("localPath"), root.localPath},
            {QStringLiteral("driveId"), root.driveId},
            {QStringLiteral("remoteRootId"), root.remoteRootId},
            {QStringLiteral("excludedPaths"), root.excludedPaths}};
}

bool Backend::setExcludedFolders(const QString &rootId, const QStringList &paths)
{
    std::unique_ptr<QDBusInterface> iface(daemon(nullptr));
    const QDBusMessage reply = iface->call(QStringLiteral("SetExcludedFolders"), rootId, paths);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        // graphsyncd not reachable: store it, the daemon applies it on start
        Config::setExcludedPaths(rootId, paths);
    }
    return true;
}

QString Backend::safeName(const QString &name)
{
    // One path component: no separators, NUL or "."/".." (names come from links and Microsoft)
    QString result = name;
    result.replace(QLatin1Char('/'), QLatin1Char('-')).replace(QLatin1Char('\\'), QLatin1Char('-')).remove(QChar(0));
    result = result.trimmed();
    if (result.isEmpty() || result == QLatin1String(".") || result == QLatin1String("..")) {
        result = QStringLiteral("_");
    }
    return result;
}

QString Backend::childPath(const QUrl &folder, const QString &name)
{
    return QDir(folder.toLocalFile()).filePath(safeName(name));
}

bool Backend::folderHasFiles(const QString &path)
{
    const QDir dir(path);
    return dir.exists() && !dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty();
}

QString Backend::personalPath() const
{
    const AccountInfo account = Config::account();
    const QString label = account.organization.isEmpty() ? organization() : Config::organizationLabel(account);
    return QDir(m_baseFolder).filePath(i18nc("folder name of the personal OneDrive, %1 is the organization", "OneDrive - %1", safeName(label)));
}

QString Backend::libraryPath(const QString &siteName, const QString &libraryName) const
{
    return libraryPathFor(Config::account(), siteName, libraryName);
}

QString Backend::libraryPathFor(const AccountInfo &account, const QString &siteName, const QString &libraryName) const
{
    const QString label = account.organization.isEmpty() ? organization() : Config::organizationLabel(account);
    return QDir(QDir(m_baseFolder).filePath(Config::sharePointFolderName(label))).filePath(safeName(siteName) + QStringLiteral(" - ") + safeName(libraryName));
}

bool Backend::pathInUse(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    const QList<RootConfig> roots = Config::roots();
    for (const RootConfig &root : roots) {
        if (clean == root.localPath || clean.startsWith(root.localPath + QLatin1Char('/')) || root.localPath.startsWith(clean + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

bool Backend::accountSignedIn(const QString &accountId) const
{
    if (!m_daemonRunning) {
        return accountId == Config::account().id && signedIn();
    }
    const QVariantList accounts = m_status.value(QStringLiteral("accounts")).toList();
    for (const QVariant &value : accounts) {
        const QVariantMap account = value.toMap();
        if (account.value(QStringLiteral("id")).toString() == accountId) {
            return account.value(QStringLiteral("signedIn")).toBool();
        }
    }
    return false;
}

QVariantMap Backend::resolveOdopen(const QString &url)
{
    // Parameters as captured in phase 0 (T5); GUIDs arrive wrapped in braces
    const QUrlQuery query{QUrl(url)};
    auto param = [&query](const char *name) {
        QString value = query.queryItemValue(QString::fromLatin1(name), QUrl::FullyDecoded);
        if (value.startsWith(QLatin1Char('{')) && value.endsWith(QLatin1Char('}'))) {
            value = value.mid(1, value.size() - 2);
        }
        return value;
    };
    QVariantMap result;
    const QString webUrl = param("webUrl");
    const QString siteId = param("siteId");
    const QString webId = param("webId");
    const QString listId = param("listId");
    // A link is untrusted input: SharePoint IDs are GUIDs and the site lives on a SharePoint host
    const QString linkHost = QUrl(webUrl).host().toLower();
    const bool sharePointHost = QUrl(webUrl).scheme() == QLatin1String("https")
        && (linkHost.endsWith(QLatin1String(".sharepoint.com")) || linkHost.endsWith(QLatin1String(".sharepoint.us"))
            || linkHost.endsWith(QLatin1String(".sharepoint.de")) || linkHost.endsWith(QLatin1String(".sharepoint.cn")));
    if (!sharePointHost || !Auth::isGuid(siteId) || !Auth::isGuid(webId) || !Auth::isGuid(listId)
        || (!param("folderId").isEmpty() && !Auth::isGuid(param("folderId")))) {
        result.insert(QStringLiteral("error"), i18n("This link does not contain a SharePoint library."));
        return result;
    }

    const QString userId = Auth::isGuid(param("userId")) ? param("userId") : QString();
    // Only shown and matched; anything that does not look like an address is left out
    static const QRegularExpression address(QStringLiteral("^[^\\s<>\"@]+@[^\\s<>\"@]+$"));
    const QString userEmail = address.match(param("userEmail")).hasMatch() ? param("userEmail") : QString();
    const QString host = QUrl(webUrl).host();
    const AccountInfo account = Config::accountForLink(userId, userEmail, host);
    const bool matches = (!userId.isEmpty() && userId.compare(account.objectId, Qt::CaseInsensitive) == 0)
        || (!userEmail.isEmpty() && userEmail.compare(account.userPrincipalName, Qt::CaseInsensitive) == 0);
    result.insert(QStringLiteral("userEmail"), userEmail);
    result.insert(QStringLiteral("accountMatches"), matches);
    result.insert(QStringLiteral("accountId"), account.id);
    result.insert(QStringLiteral("accountName"), account.userPrincipalName);
    // The link names someone not added here: offer to add that account
    if (!matches && !userEmail.isEmpty()) {
        result.insert(QStringLiteral("addAccountEmail"), userEmail);
    }
    if (account.isValid() && !accountSignedIn(account.id)) {
        result.insert(QStringLiteral("signInAccountId"), account.id);
        result.insert(QStringLiteral("error"), i18n("This link is for %1, which is signed out. Sign in to it first.", account.userPrincipalName));
        return result;
    }

    const GraphResponse site = get(QStringLiteral("/sites/%1,%2,%3?$select=id,displayName,webUrl").arg(host, siteId, webId), account.id);
    if (!site.ok()) {
        result.insert(QStringLiteral("error"), i18n("The site could not be opened: %1", site.describe()));
        return result;
    }
    const QString siteGraphId = site.json().value(QLatin1String("id")).toString();
    // Names from Microsoft, not from the link (webTitle/listTitle could say anything)
    const QString siteName = site.json().value(QLatin1String("displayName")).toString();
    const GraphResponse drive = get(QStringLiteral("/sites/%1/lists/%2/drive?$select=id,name,webUrl").arg(siteGraphId, listId), account.id);
    if (!drive.ok()) {
        result.insert(QStringLiteral("error"), i18n("The library could not be opened: %1", drive.describe()));
        return result;
    }
    const QString libraryName = drive.json().value(QLatin1String("name")).toString();
    result.insert(QStringLiteral("siteName"), siteName);
    result.insert(QStringLiteral("libraryName"), libraryName);
    result.insert(QStringLiteral("driveId"), drive.json().value(QLatin1String("id")).toString());
    result.insert(QStringLiteral("webUrl"), drive.json().value(QLatin1String("webUrl")).toString());
    result.insert(QStringLiteral("kind"), QStringLiteral("library"));
    result.insert(QStringLiteral("name"), QString(siteName + QStringLiteral(" - ") + libraryName));
    result.insert(QStringLiteral("localPath"), libraryPathFor(account, siteName, libraryName));

    // Folder level (scope=OPENFOLDER): folderId is the item's UniqueId GUID (phase 0 T6)
    const QString folderId = param("folderId");
    if (param("scope") == QLatin1String("OPENFOLDER") && !folderId.isEmpty()) {
        const GraphResponse folder =
            get(QStringLiteral("/sites/%1/lists/%2/items/%3/driveItem?$select=id,name,webUrl").arg(siteGraphId, listId, folderId), account.id);
        if (!folder.ok()) {
            result.insert(QStringLiteral("error"), i18n("The folder could not be opened: %1", folder.describe()));
            return result;
        }
        const QString folderName = folder.json().value(QLatin1String("name")).toString();
        result.insert(QStringLiteral("folderName"), folderName);
        result.insert(QStringLiteral("remoteRootId"), folder.json().value(QLatin1String("id")).toString());
        result.insert(QStringLiteral("webUrl"), folder.json().value(QLatin1String("webUrl")).toString());
        result.insert(QStringLiteral("kind"), QStringLiteral("folder"));
        result.insert(QStringLiteral("name"), QString(siteName + QStringLiteral(" - ") + folderName));
        result.insert(QStringLiteral("localPath"), libraryPathFor(account, siteName, folderName));
    }
    return result;
}

QString Backend::addRoot(const QVariantMap &root)
{
    QVariantMap request = root;
    // The account whose sign-in listed the library (Add a Library, the wizard), not whichever
    // account is current by the time it is saved
    if (request.value(QStringLiteral("accountId")).toString().isEmpty()) {
        const AuthConfig listing = m_auth->config();
        request.insert(QStringLiteral("accountId"), GraphCore::Accounts::findUser(listing.tenantId, listing.objectId).id);
    }
    QString path = request.value(QStringLiteral("localPath")).toString();
    if (path.startsWith(QLatin1String("file://"))) {
        path = QUrl(path).toLocalFile();
    }
    request.insert(QStringLiteral("localPath"), QDir::cleanPath(path));
    switch (Config::rootPathProblem(path)) {
    case Config::RootPathProblem::None:
        break;
    case Config::RootPathProblem::NotAbsolute:
        setError(i18n("Choose a full folder path, such as %1.", libraryPath(i18n("Site"), i18n("Library"))));
        return {};
    case Config::RootPathProblem::TooBroad:
        setError(i18n("%1 cannot be synced as a whole: everything in it would be uploaded. Choose a folder inside it.", path));
        return {};
    case Config::RootPathProblem::Overlaps:
        setError(i18n("%1 is already used by another synced library.", path));
        return {};
    }
    std::unique_ptr<QDBusInterface> iface(daemon(nullptr));
    const QDBusReply<QString> reply =
        iface->call(QStringLiteral("AddRoot"), QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(request)).toJson(QJsonDocument::Compact)));
    if (!reply.isValid() || reply.value().isEmpty()) {
        setError(reply.isValid() ? i18n("The sync service refused this library.") : i18n("The sync service is not running: %1", reply.error().message()));
        return {};
    }
    setError({});
    return reply.value();
}

void Backend::finishSetup()
{
    GeneralConfig general = Config::general();
    general.setupDone = true;
    general.baseFolder = m_baseFolder;
    Config::setGeneral(general);
    // Start graphsyncd at every login from now on (the unit is installed with the package)
    QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("enable"), QStringLiteral("plasma-graphsync-linux.service")});
}

}
