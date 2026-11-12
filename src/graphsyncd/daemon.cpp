// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "daemon.h"
#include "foldericon.h"
#include "places.h"
#include "placeholder.h"
#include "graphsync_debug.h"
#include "rootcontroller.h"

#include <KLocalizedString>
#include <KNotification>
#include <KSystemClipboard>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QMimeData>
#include <QNetworkInformation>
#include <QProcess>

#include <algorithm>

namespace GraphSync
{

namespace
{
const QString componentName = QStringLiteral("plasma-graphsync-linux");
constexpr int errorRenotifySeconds = 60 * 60;

bool notificationsAvailable()
{
    // KNotification needs a GUI application; graphsyncd falls back to a core application without a display
    return qobject_cast<QGuiApplication *>(QCoreApplication::instance()) != nullptr;
}
}

Daemon::Daemon(QObject *parent)
    : QObject(parent)
{
    m_resumeTimer.setSingleShot(true);
    connect(&m_resumeTimer, &QTimer::timeout, this, &Daemon::resume);

    if (QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Metered)) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::isMeteredChanged, this, &Daemon::updateMetered);
        // Back online (the NetworkManager backend reports reachability too): sync at once
        // instead of waiting for the next retry of the offline locations
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this, [this](QNetworkInformation::Reachability reachability) {
            if (reachability == QNetworkInformation::Reachability::Online) {
                syncNow();
            }
        });
    }
    reloadAccount();
    updateMetered();
}

Daemon::~Daemon()
{
    dropAccounts();
}

std::shared_ptr<Auth> Daemon::authFor(const QString &accountId)
{
    if (std::shared_ptr<Auth> auth = m_auths.value(accountId)) {
        return auth;
    }
    const AccountInfo account = GraphCore::Accounts::find(accountId);
    if (!account.isValid()) {
        return {};
    }
    auto auth = std::make_shared<Auth>(Config::authFor(account));
    m_auths.insert(accountId, auth);
    return auth;
}

void Daemon::dropAccounts()
{
    stopRoots();
    m_auths.clear();
}

void Daemon::reloadAccount()
{
    if (m_reloading) {
        m_reloadPending = true;
        return;
    }
    m_reloading = true;
    do {
        m_reloadPending = false;
        reloadOnce();
    } while (m_reloadPending);
    m_reloading = false;
}

void Daemon::reloadOnce()
{
    // Sign-ins, accounts and their roots may all have changed (app, graphsyncctl, another app of
    // the suite): start over
    dropAccounts();
    invalidateStatusCache();
    // Collected first and swapped in at the end: a status asked for meanwhile (the KWallet reads
    // run local event loops) keeps showing the accounts as they were, not all signed out
    QHash<QString, bool> signedIn;
    const QList<AccountInfo> accounts = GraphCore::Accounts::all();
    for (const AccountInfo &account : accounts) {
        const std::shared_ptr<Auth> auth = authFor(account.id);
        signedIn.insert(account.id, auth && auth->isConfigured() && auth->hasStoredRefreshToken());
    }
    m_accountSignedIn = signedIn;
    // No current account (signed out with graphsyncctl) or a signed-out one: another signed-in
    // account takes over
    chooseCurrentAccount();
    m_signedOutNotified.clear();
    if (!m_signedIn) {
        qCInfo(GRAPHSYNC) << "Not signed in; waiting for the setup wizard";
    }
    startRoots();
    Q_EMIT statusChanged();
}

void Daemon::reloadOneAccount(const QString &accountId)
{
    if (m_reloading) {
        // A full reload is running: it covers this account too
        m_reloadPending = true;
        return;
    }
    m_reloading = true;
    // Only this account's locations restart (a new sign-in): the other accounts' transfers go on
    const QStringList rootIds = m_rootAccounts.keys(accountId);
    for (const QString &rootId : rootIds) {
        delete m_roots.take(rootId);
        m_rootAccounts.remove(rootId);
    }
    m_auths.remove(accountId);
    invalidateStatusCache();
    const std::shared_ptr<Auth> auth = authFor(accountId);
    m_accountSignedIn.insert(accountId, auth && auth->isConfigured() && auth->hasStoredRefreshToken());
    m_signedOutNotified.remove(accountId);
    chooseCurrentAccount();
    startRoots();
    m_reloading = false;
    Q_EMIT rootsChanged();
    Q_EMIT statusChanged();
    if (m_reloadPending) {
        reloadAccount();
    }
}

void Daemon::signOut(const QString &given)
{
    const QString accountId = given.isEmpty() ? Config::account().id : given;
    const QStringList rootIds = m_rootAccounts.keys(accountId);
    for (const QString &rootId : rootIds) {
        delete m_roots.take(rootId);
        m_rootAccounts.remove(rootId);
    }
    // Also an account not loaded yet (signed in since the last reload): its token must go too
    if (const std::shared_ptr<Auth> auth = authFor(accountId)) {
        // Also clears the current account when it was this one
        auth->logout();
    }
    m_auths.remove(accountId);
    invalidateStatusCache();
    m_accountSignedIn.insert(accountId, false);
    chooseCurrentAccount();
    Q_EMIT rootsChanged();
    Q_EMIT statusChanged();
}

void Daemon::removeAccount(const QString &accountId)
{
    if (accountId.isEmpty()) {
        return;
    }
    signOut(accountId);
    // Its locations go (shortcuts with their root); the local copies stay as ordinary files
    const QList<RootConfig> roots = Config::roots();
    for (const RootConfig &root : roots) {
        if (root.accountId == accountId && root.kind != QLatin1String("shortcut")) {
            removeRoot(root.id);
        }
    }
    qCInfo(GRAPHSYNC).noquote() << "Removed account" << accountId << "from this computer";
    Q_EMIT statusChanged();
}

bool Daemon::setCurrentAccount(const QString &accountId)
{
    if (!GraphCore::Accounts::find(accountId).isValid()) {
        return false;
    }
    invalidateStatusCache();
    Config::setCurrentAccountId(accountId);
    m_signedIn = m_accountSignedIn.value(accountId);
    Q_EMIT statusChanged();
    return true;
}

void Daemon::chooseCurrentAccount()
{
    const QString current = Config::account().id;
    if (!current.isEmpty() && m_accountSignedIn.value(current)) {
        m_signedIn = true;
        return;
    }
    for (auto it = m_accountSignedIn.constBegin(); it != m_accountSignedIn.constEnd(); ++it) {
        if (it.value()) {
            Config::setCurrentAccountId(it.key());
            m_signedIn = true;
            return;
        }
    }
    m_signedIn = false;
}

void Daemon::startRoots()
{
    const QList<RootConfig> roots = Config::roots();
    QSet<QString> ids;
    for (const RootConfig &root : roots) {
        ids.insert(root.id);
    }
    QSet<QString> signedOut;
    for (const RootConfig &root : roots) {
        if (root.kind == QLatin1String("shortcut") && !ids.contains(root.parentRootId)) {
            // Its root was removed while we were not running
            Config::removeRoot(root.id);
            continue;
        }
        if (!root.enabled || m_roots.contains(root.id)) {
            continue;
        }
        // Set up while signed out: the next account that signs in claims it
        const std::shared_ptr<Auth> auth = root.accountId.isEmpty() ? nullptr : authFor(root.accountId);
        if (!auth || !auth->hasStoredRefreshToken()) {
            signedOut.insert(root.accountId);
            continue;
        }
        createController(root, auth);
    }
    for (const QString &accountId : std::as_const(signedOut)) {
        qCInfo(GRAPHSYNC).noquote() << "Locations of account" << (accountId.isEmpty() ? QStringLiteral("(none)") : accountId) << "wait for a sign-in";
    }
    applyPause();
}

void Daemon::stopRoots()
{
    qDeleteAll(m_roots);
    m_roots.clear();
    m_rootAccounts.clear();
    Q_EMIT rootsChanged();
}

namespace
{
struct Place {
    QString path;
    QString label;
    QString icon;
};

// Libraries share one entry for the folder that holds them
// (<base>/SharePoint - <Organisation>/<Site> - <Library>); the personal OneDrive gets its own
Place placeFor(const RootConfig &root)
{
    if (root.kind != QLatin1String("personal")) {
        const QString parent = QFileInfo(root.localPath).absolutePath();
        const AccountInfo account = GraphCore::Accounts::find(root.accountId);
        // "SharePoint - <Organisation>", or with the address for a second user of the organisation;
        // never another organisation whose name starts the same ("Contoso Group" for "Contoso")
        const QString folderName = QFileInfo(parent).fileName();
        const QString expected = Config::sharePointFolderName(account.organization);
        if (!account.organization.isEmpty() && (folderName == expected || folderName.startsWith(expected + QStringLiteral(" (")))) {
            return {parent, folderName, QStringLiteral("folder-network")};
        }
    }
    return {root.localPath, root.name, QStringLiteral("folder-cloud")};
}
}

RootController *Daemon::createController(const RootConfig &root, std::shared_ptr<Auth> auth)
{
    // Shortcuts live inside the OneDrive folder, which has its own Places entry
    if (root.kind != QLatin1String("shortcut")) {
        const Place place = placeFor(root);
        if (place.path != root.localPath) {
            QDir().mkpath(place.path);
            FolderIcon::apply(place.path, place.icon);
        }
        Places::ensure(place.path, place.label, place.icon);
    }
    qCInfo(GRAPHSYNC) << "Syncing" << root.name << "->" << root.localPath;
    auto *controller = new RootController(root, std::move(auth), this);
    m_roots.insert(root.id, controller);
    m_rootAccounts.insert(root.id, root.accountId);
    connect(controller, &RootController::statusChanged, this, &Daemon::statusChanged);
    connect(controller, &RootController::activityEntry, this, &Daemon::activityAdded);
    connect(controller, &RootController::conflictCreated, this, &Daemon::notifyConflict);
    connect(controller, &RootController::errorOccurred, this, [this, name = root.name](const QString &message) {
        notifyError(name, message);
    });
    connect(controller, &RootController::bigDeletePending, this, [this, id = root.id, name = root.name](int missing, int known) {
        notifyBigDelete(id, name, missing, known);
    });
    connect(controller, &RootController::signInLost, this, [this, accountId = root.accountId] {
        m_accountSignedIn.insert(accountId, false);
        if (accountId == Config::account().id) {
            m_signedIn = false;
        }
        notifySignedOut(accountId);
        Q_EMIT statusChanged();
    });
    connect(controller, &RootController::hydrated, this, [this, controller](const QString &rel, bool ok, const QString &error) {
        const QString path = controller->localPath() + QLatin1Char('/') + rel;
        m_hydrations.insert(path, ok ? QStringLiteral("done") : QStringLiteral("failed: ") + error);
        if (!ok && notificationsAvailable()) {
            auto *notification = new KNotification(QStringLiteral("syncError"));
            notification->setComponentName(componentName);
            notification->setTitle(i18n("Could not download %1", QFileInfo(path).fileName()));
            notification->setText(error);
            notification->sendEvent();
        }
    });
    connect(controller, &RootController::shortcutsChanged, this, [this, id = root.id](const QVariantList &shortcuts, const QStringList &removedPaths) {
        reconcileShortcuts(id, shortcuts, removedPaths);
    });
    Q_EMIT rootsChanged();
    return controller;
}

QString Daemon::addRoot(const RootConfig &root)
{
    if (Config::rootPathProblem(root.localPath) != Config::RootPathProblem::None) {
        qCWarning(GRAPHSYNC).noquote() << "Refused a sync location that is not absolute, too broad or overlapping:" << root.localPath;
        return {};
    }
    RootConfig wanted = root;
    if (wanted.accountId.isEmpty()) {
        // From the app: the account it works with
        wanted.accountId = Config::account().id;
    }
    invalidateStatusCache();
    const RootConfig stored = Config::addRoot(wanted);
    const std::shared_ptr<Auth> auth = stored.accountId.isEmpty() ? nullptr : authFor(stored.accountId);
    if (auth && auth->hasStoredRefreshToken()) {
        createController(stored, auth);
        applyPause();
    }
    if (notificationsAvailable()) {
        auto *notification = new KNotification(QStringLiteral("rootAdded"));
        notification->setComponentName(componentName);
        notification->setTitle(i18n("Syncing %1", stored.name));
        notification->setText(i18n("Files are kept in %1", stored.localPath));
        notification->sendEvent();
    }
    Q_EMIT statusChanged();
    return stored.id;
}

bool Daemon::removeRoot(const QString &id)
{
    if (RootController *controller = m_roots.take(id)) {
        delete controller;
        m_rootAccounts.remove(id);
        Q_EMIT rootsChanged();
    }
    // Its shortcuts stop too; like the root's own files, their local copies are kept
    const QList<RootConfig> all = Config::roots();
    for (const RootConfig &root : all) {
        if (root.kind == QLatin1String("shortcut") && root.parentRootId == id) {
            delete m_roots.take(root.id);
            m_rootAccounts.remove(root.id);
            Config::removeRoot(root.id);
        }
    }

    // Drop the Places entry unless another library still lives in the same SharePoint folder
    const QList<RootConfig> roots = Config::roots();
    QString placePath;
    bool shared = false;
    for (const RootConfig &root : roots) {
        if (root.id == id) {
            placePath = placeFor(root).path;
        }
    }
    for (const RootConfig &root : roots) {
        if (root.id != id && placeFor(root).path == placePath) {
            shared = true;
        }
    }
    if (!placePath.isEmpty() && !shared) {
        Places::remove(placePath);
    }

    invalidateStatusCache();
    const bool removed = Config::removeRoot(id);
    Q_EMIT statusChanged();
    return removed;
}

void Daemon::setExcludedFolders(const QString &id, const QStringList &paths)
{
    Config::setExcludedPaths(id, paths);
    if (RootController *controller = m_roots.value(id)) {
        controller->setExcludedPaths(paths);
    }
}

void Daemon::resolveBigDelete(const QString &id, const QString &action)
{
    if (RootController *controller = m_roots.value(id)) {
        controller->resolveBigDelete(action);
    }
}

void Daemon::syncNow()
{
    for (RootController *controller : std::as_const(m_roots)) {
        controller->syncNow();
    }
}

void Daemon::pause(int minutes)
{
    m_userPaused = true;
    if (minutes > 0) {
        m_pausedUntil = QDateTime::currentDateTimeUtc().addSecs(qint64(minutes) * 60);
        m_resumeTimer.start(std::chrono::minutes(minutes));
    } else {
        m_pausedUntil = {};
        m_resumeTimer.stop();
    }
    applyPause();
}

void Daemon::resume()
{
    m_userPaused = false;
    m_pausedUntil = {};
    m_resumeTimer.stop();
    applyPause();
}

void Daemon::updateMetered()
{
    const auto *info = QNetworkInformation::instance();
    const bool metered = info && info->isMetered() && Config::general().pauseOnMeteredNetwork;
    if (metered != m_meteredPaused) {
        m_meteredPaused = metered;
        qCInfo(GRAPHSYNC) << (metered ? "Metered network, pausing" : "No longer on a metered network");
        applyPause();
    }
}

void Daemon::applyPause()
{
    const bool paused = m_userPaused || m_meteredPaused;
    for (RootController *controller : std::as_const(m_roots)) {
        controller->setPaused(paused);
    }
    Q_EMIT statusChanged();
}

void Daemon::invalidateStatusCache()
{
    m_statusCacheAge.invalidate();
}

void Daemon::refreshStatusCache() const
{
    // Status is asked for after every change of every location (throttled, but continuously during
    // a big sync); graphsyncrc and graphaccountsrc are reread at most every two seconds, and at once
    // after this daemon changed them
    if (m_statusCacheAge.isValid() && !m_statusCacheAge.hasExpired(2000)) {
        return;
    }
    m_cachedCurrent = Config::account();
    m_cachedRoots = Config::roots();
    m_cachedAccounts = GraphCore::Accounts::all();
    m_cachedGeneral = Config::general();
    m_statusCacheAge.start();
}

QJsonObject Daemon::status() const
{
    refreshStatusCache();
    const AccountInfo account = m_cachedCurrent;
    const GeneralConfig &general = m_cachedGeneral;
    QJsonArray roots;
    // Personal OneDrive first, then libraries by name, like the OneDrive settings list
    QList<RootController *> ordered = m_roots.values();
    std::sort(ordered.begin(), ordered.end(), [](RootController *a, RootController *b) {
        const QVariantMap sa = a->status();
        const QVariantMap sb = b->status();
        const bool pa = sa.value(QStringLiteral("kind")) == QLatin1String("personal");
        const bool pb = sb.value(QStringLiteral("kind")) == QLatin1String("personal");
        if (pa != pb) {
            return pa;
        }
        return sa.value(QStringLiteral("name")).toString().localeAwareCompare(sb.value(QStringLiteral("name")).toString()) < 0;
    });
    for (const RootController *controller : std::as_const(ordered)) {
        QVariantMap root = controller->status();
        root.insert(QStringLiteral("accountId"), m_rootAccounts.value(root.value(QStringLiteral("id")).toString()));
        roots.append(QJsonObject::fromVariantMap(root));
    }
    // Locations whose account is signed out do not run, but stay in the list, waiting for it
    const QList<RootConfig> &configured = m_cachedRoots;
    QHash<QString, int> locations;
    for (const RootConfig &root : configured) {
        if (root.kind == QLatin1String("shortcut")) {
            continue;
        }
        locations[root.accountId] += 1;
        if (!root.enabled || m_roots.contains(root.id)) {
            continue;
        }
        roots.append(QJsonObject{{QStringLiteral("id"), root.id},
                                 {QStringLiteral("name"), root.name},
                                 {QStringLiteral("localPath"), root.localPath},
                                 {QStringLiteral("kind"), root.kind},
                                 {QStringLiteral("webUrl"), root.webUrl},
                                 {QStringLiteral("accountId"), root.accountId},
                                 {QStringLiteral("filesOnDemand"), root.filesOnDemand},
                                 {QStringLiteral("excludedPaths"), QJsonArray::fromStringList(root.excludedPaths)},
                                 // Not running although its account is signed in: starting (a reload)
                                 {QStringLiteral("state"), m_accountSignedIn.value(root.accountId) ? QStringLiteral("waiting") : QStringLiteral("signedOut")}});
    }
    // Every account on this computer (also those only another app of the suite uses): who is
    // signed in here, for the Accounts page and the tray
    QJsonArray accounts;
    const QList<AccountInfo> &known = m_cachedAccounts;
    for (const AccountInfo &each : known) {
        accounts.append(QJsonObject{{QStringLiteral("id"), each.id},
                                    {QStringLiteral("displayName"), each.displayName},
                                    {QStringLiteral("userPrincipalName"), each.userPrincipalName},
                                    {QStringLiteral("organization"), each.organization},
                                    {QStringLiteral("tenantId"), each.tenantId},
                                    {QStringLiteral("current"), each.id == account.id},
                                    {QStringLiteral("signedIn"), m_accountSignedIn.value(each.id)},
                                    {QStringLiteral("locations"), locations.value(each.id)}});
    }
    return QJsonObject{
        {QStringLiteral("version"), QCoreApplication::applicationVersion()},
        {QStringLiteral("signedIn"), m_signedIn},
        {QStringLiteral("setupDone"), general.setupDone},
        {QStringLiteral("baseFolder"), general.baseFolder},
        {QStringLiteral("pauseOnMeteredNetwork"), general.pauseOnMeteredNetwork},
        {QStringLiteral("accounts"), accounts},
        {QStringLiteral("account"),
         QJsonObject{{QStringLiteral("id"), account.id},
                     {QStringLiteral("displayName"), account.displayName},
                     {QStringLiteral("userPrincipalName"), account.userPrincipalName},
                     {QStringLiteral("organization"), account.organization}}},
        {QStringLiteral("paused"), m_userPaused || m_meteredPaused},
        {QStringLiteral("pausedByUser"), m_userPaused},
        {QStringLiteral("pausedForMeteredNetwork"), m_meteredPaused},
        {QStringLiteral("pausedUntil"), m_pausedUntil.isValid() ? m_pausedUntil.toString(Qt::ISODate) : QString()},
        {QStringLiteral("roots"), roots},
    };
}

QJsonArray Daemon::activity() const
{
    QVariantList all;
    for (const RootController *controller : std::as_const(m_roots)) {
        all.append(controller->activity());
    }
    std::sort(all.begin(), all.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("time")).toString() > b.toMap().value(QStringLiteral("time")).toString();
    });
    QJsonArray result;
    for (qsizetype i = 0; i < std::min<qsizetype>(all.size(), 100); ++i) {
        result.append(QJsonObject::fromVariantMap(all.at(i).toMap()));
    }
    return result;
}

// ---------------------------------------------------------------------------------------
// Notifications

void Daemon::launchApp(const QStringList &arguments)
{
    QProcess::startDetached(QStringLiteral(GRAPHSYNC_BINDIR "/plasma-graphsync-linux"), arguments);
}

void Daemon::reconcileShortcuts(const QString &parentId, const QVariantList &shortcuts, const QStringList &removedPaths)
{
    RootController *parent = m_roots.value(parentId);
    if (!parent) {
        return;
    }
    const QString parentPath = parent->localPath();
    // Stable ids: a shortcut keeps its state database across restarts, renames and moves
    const auto rootIdFor = [&parentId](const QString &itemId) {
        return QStringLiteral("s") + QString::fromLatin1(QCryptographicHash::hash((parentId + itemId).toUtf8(), QCryptographicHash::Sha1).toHex().left(7));
    };

    QHash<QString, RootConfig> existing;
    const QList<RootConfig> all = Config::roots();
    for (const RootConfig &root : all) {
        if (root.kind == QLatin1String("shortcut") && root.parentRootId == parentId) {
            existing.insert(root.id, root);
        }
    }

    bool changed = false;
    QSet<QString> wanted;
    QSet<QString> wantedPaths;
    for (const QVariant &value : shortcuts) {
        const QVariantMap shortcut = value.toMap();
        RootConfig root;
        root.id = rootIdFor(shortcut.value(QStringLiteral("id")).toString());
        root.name = shortcut.value(QStringLiteral("name")).toString();
        root.localPath = QDir::cleanPath(parentPath + QLatin1Char('/') + shortcut.value(QStringLiteral("rel")).toString());
        root.driveId = shortcut.value(QStringLiteral("driveId")).toString();
        root.remoteRootId = shortcut.value(QStringLiteral("remoteId")).toString();
        root.webUrl = shortcut.value(QStringLiteral("webUrl")).toString();
        root.kind = QStringLiteral("shortcut");
        root.parentRootId = parentId;
        root.accountId = m_rootAccounts.value(parentId);
        wanted.insert(root.id);
        wantedPaths.insert(root.localPath);

        const auto old = existing.constFind(root.id);
        const bool unchanged = old != existing.constEnd() && old->localPath == root.localPath && old->name == root.name && old->driveId == root.driveId
            && old->remoteRootId == root.remoteRootId;
        if (unchanged && m_roots.contains(root.id)) {
            continue;
        }
        // New, or renamed or moved: (re)start on the current path; the state database stays
        delete m_roots.take(root.id);
        Config::addRoot(root);
        qCInfo(GRAPHSYNC) << "Shortcut" << root.name << "->" << root.localPath;
        // The parent's own sign-in (it keeps it alive): never a root without one
        createController(root, parent->auth());
        changed = true;
    }
    for (auto it = existing.constBegin(); it != existing.constEnd(); ++it) {
        if (!wanted.contains(it.key())) {
            delete m_roots.take(it.key());
            m_rootAccounts.remove(it.key());
            Config::removeRoot(it.key());
            changed = true;
        }
    }
    // Removed shortcuts: their folder goes to the Trash, as OneDrive does
    for (const QString &rel : removedPaths) {
        const QString path = QDir::cleanPath(parentPath + QLatin1Char('/') + rel);
        if (!wantedPaths.contains(path) && QFileInfo(path).isDir() && QFile::moveToTrash(path)) {
            qCInfo(GRAPHSYNC) << "Moved the folder of a removed shortcut to the Trash:" << path;
        }
    }
    if (changed) {
        applyPause();
        Q_EMIT rootsChanged();
        Q_EMIT statusChanged();
    }
}

RootController *Daemon::controllerFor(const QString &path, QString *rel) const
{
    const QString clean = QDir::cleanPath(path);
    RootController *owner = nullptr;
    for (RootController *controller : m_roots) {
        const QString root = controller->localPath();
        if ((clean == root || clean.startsWith(root + QLatin1Char('/'))) && (!owner || root.size() > owner->localPath().size())) {
            owner = controller;
        }
    }
    if (owner) {
        *rel = clean == owner->localPath() ? QString() : clean.mid(owner->localPath().size() + 1);
    }
    return owner;
}

bool Daemon::hydrate(const QString &path)
{
    const QString real = Placeholder::realPathOf(QDir::cleanPath(path));
    QString rel;
    RootController *controller = controllerFor(real, &rel);
    if (!controller || rel.isEmpty()) {
        return false;
    }
    m_hydrations.insert(real, QStringLiteral("pending"));
    controller->hydrate(rel);
    return true;
}

QString Daemon::hydrationState(const QString &path) const
{
    return m_hydrations.value(Placeholder::realPathOf(QDir::cleanPath(path)));
}

void Daemon::pin(const QStringList &paths, const QString &state)
{
    const StateDb::Pin pin = state == QLatin1String("online") ? StateDb::Pin::OnlineOnly : StateDb::Pin::AlwaysKeep;
    for (const QString &path : paths) {
        QString rel;
        // A placeholder stands for its real file
        const QString real = Placeholder::hasSuffix(path) && Placeholder::isPlaceholder(path) ? Placeholder::realPathOf(path) : QDir::cleanPath(path);
        if (RootController *controller = controllerFor(real, &rel)) {
            controller->pin(rel, pin);
        }
    }
}

bool Daemon::copyToClipboard(const QString &text)
{
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        return false;
    }
    auto *mime = new QMimeData;
    mime->setText(text);
    KSystemClipboard::instance()->setMimeData(mime, QClipboard::Clipboard);
    return true;
}

void Daemon::notifyLinkCopied(const QString &path, const QString &name, const QString &scope, const QString &type)
{
    if (!notificationsAvailable()) {
        return;
    }
    const bool edit = type == QLatin1String("edit");
    QString text;
    if (scope == QLatin1String("anonymous")) {
        text = edit ? i18n("Anyone with the link can edit %1.", name) : i18n("Anyone with the link can view %1.", name);
    } else if (scope == QLatin1String("organization")) {
        QString rel;
        const RootController *owner = controllerFor(path, &rel);
        const QString organization = GraphCore::Accounts::find(owner ? m_rootAccounts.value(owner->id()) : Config::account().id).organization;
        if (organization.isEmpty()) {
            text = edit ? i18n("People in your organisation with the link can edit %1.", name) : i18n("People in your organisation with the link can view %1.", name);
        } else {
            text = edit ? i18n("People in %1 with the link can edit %2.", organization, name) : i18n("People in %1 with the link can view %2.", organization, name);
        }
    } else {
        text = i18n("Only people who already have access can open %1 with this link.", name);
    }
    auto *notification = new KNotification(QStringLiteral("linkCopied"));
    notification->setComponentName(componentName);
    notification->setTitle(i18n("Link copied"));
    notification->setText(text);
    KNotificationAction *settings = notification->addAction(i18n("Link Settings…"));
    connect(settings, &KNotificationAction::activated, this, [path] {
        launchApp({QStringLiteral("--share"), path});
    });
    notification->sendEvent();
}

void Daemon::notifyLinkFailed(const QString &name, const QString &error)
{
    if (!notificationsAvailable()) {
        return;
    }
    auto *notification = new KNotification(QStringLiteral("linkError"));
    notification->setComponentName(componentName);
    notification->setTitle(i18n("Could not copy a link to %1", name));
    notification->setText(error);
    notification->sendEvent();
}

void Daemon::notifyConflict(const QString &path, const QString &copyName)
{
    if (!notificationsAvailable()) {
        return;
    }
    auto *notification = new KNotification(QStringLiteral("conflict"));
    notification->setComponentName(componentName);
    notification->setTitle(i18n("Sync conflict"));
    notification->setText(i18n("%1 was changed here and online. Both versions were kept; your version is now %2.",
                               QDir(path).dirName(),
                               copyName));
    KNotificationAction *open = notification->addAction(i18n("Show in Folder"));
    connect(open, &KNotificationAction::activated, this, [path] {
        QProcess::startDetached(QStringLiteral("dolphin"), {QStringLiteral("--select"), path});
    });
    notification->sendEvent();
}

void Daemon::notifyError(const QString &root, const QString &message)
{
    const QString key = root + QLatin1Char('\n') + message;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto last = m_notifiedErrors.constFind(key);
    if (last != m_notifiedErrors.constEnd() && last->secsTo(now) < errorRenotifySeconds) {
        return;
    }
    m_notifiedErrors.insert(key, now);
    if (!notificationsAvailable()) {
        return;
    }
    auto *notification = new KNotification(QStringLiteral("syncError"));
    notification->setComponentName(componentName);
    notification->setTitle(i18n("Problem syncing %1", root));
    notification->setText(message);
    notification->sendEvent();
}

void Daemon::notifyBigDelete(const QString &rootId, const QString &rootName, int missing, int known)
{
    if (!notificationsAvailable()) {
        return;
    }
    auto *notification = new KNotification(QStringLiteral("bigDelete"), KNotification::Persistent);
    notification->setComponentName(componentName);
    notification->setTitle(i18n("Many files were removed from %1", rootName));
    notification->setText(i18n("%1 of %2 synced items are gone from this computer. Nothing has been deleted online yet.", missing, known));
    KNotificationAction *remove = notification->addAction(i18n("Delete Online Too"));
    connect(remove, &KNotificationAction::activated, this, [this, rootId] {
        resolveBigDelete(rootId, QStringLiteral("delete"));
    });
    KNotificationAction *restore = notification->addAction(i18n("Restore Files"));
    connect(restore, &KNotificationAction::activated, this, [this, rootId] {
        resolveBigDelete(rootId, QStringLiteral("restore"));
    });
    notification->sendEvent();
}

void Daemon::notifySignedOut(const QString &accountId)
{
    if (m_signedOutNotified.contains(accountId) || !notificationsAvailable()) {
        return;
    }
    m_signedOutNotified.insert(accountId);
    const AccountInfo account = GraphCore::Accounts::find(accountId);
    auto *notification = new KNotification(QStringLiteral("signedOut"), KNotification::Persistent);
    notification->setComponentName(componentName);
    notification->setTitle(i18n("Sign in again"));
    notification->setText(account.userPrincipalName.isEmpty() ? i18n("Your session expired, so files are not syncing.")
                                                              : i18n("The session of %1 expired, so its files are not syncing.", account.userPrincipalName));
    KNotificationAction *signIn = notification->addAction(i18n("Sign In"));
    connect(signIn, &KNotificationAction::activated, this, [this, accountId] {
        // The app signs in the account it works with: make it this one
        setCurrentAccount(accountId);
        launchApp({QStringLiteral("--sign-in")});
    });
    notification->sendEvent();
}

}
