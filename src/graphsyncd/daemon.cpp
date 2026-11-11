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
    stopRoots();
}

void Daemon::reloadAccount()
{
    stopRoots();
    m_auth = std::make_unique<Auth>(Config::auth());
    m_signedIn = m_auth->isConfigured() && m_auth->hasStoredRefreshToken();
    m_signedOutNotified = false;
    if (m_signedIn) {
        startRoots();
    } else {
        qCInfo(GRAPHSYNC) << "Not signed in; waiting for the setup wizard";
    }
    Q_EMIT statusChanged();
}

void Daemon::signOut()
{
    stopRoots();
    if (m_auth) {
        m_auth->logout();
    }
    m_signedIn = false;
    Q_EMIT statusChanged();
}

void Daemon::startRoots()
{
    const QList<RootConfig> roots = Config::roots();
    QSet<QString> ids;
    for (const RootConfig &root : roots) {
        ids.insert(root.id);
    }
    for (const RootConfig &root : roots) {
        if (root.kind == QLatin1String("shortcut") && !ids.contains(root.parentRootId)) {
            // Its root was removed while we were not running
            Config::removeRoot(root.id);
            continue;
        }
        if (root.enabled && !m_roots.contains(root.id)) {
            createController(root);
        }
    }
    applyPause();
}

void Daemon::stopRoots()
{
    qDeleteAll(m_roots);
    m_roots.clear();
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
        const AccountInfo account = Config::account();
        const QString folderName = Config::sharePointFolderName(account.organization);
        if (!account.organization.isEmpty() && QFileInfo(parent).fileName() == folderName) {
            return {parent, folderName, QStringLiteral("folder-network")};
        }
    }
    return {root.localPath, root.name, QStringLiteral("folder-cloud")};
}
}

RootController *Daemon::createController(const RootConfig &root)
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
    auto *controller = new RootController(root, m_auth.get(), this);
    m_roots.insert(root.id, controller);
    connect(controller, &RootController::statusChanged, this, &Daemon::statusChanged);
    connect(controller, &RootController::activityEntry, this, &Daemon::activityAdded);
    connect(controller, &RootController::conflictCreated, this, &Daemon::notifyConflict);
    connect(controller, &RootController::errorOccurred, this, [this, name = root.name](const QString &message) {
        notifyError(name, message);
    });
    connect(controller, &RootController::bigDeletePending, this, [this, id = root.id, name = root.name](int missing, int known) {
        notifyBigDelete(id, name, missing, known);
    });
    connect(controller, &RootController::signInLost, this, &Daemon::notifySignedOut);
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
    const RootConfig stored = Config::addRoot(root);
    if (m_signedIn) {
        createController(stored);
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
        Q_EMIT rootsChanged();
    }
    // Its shortcuts stop too; like the root's own files, their local copies are kept
    const QList<RootConfig> all = Config::roots();
    for (const RootConfig &root : all) {
        if (root.kind == QLatin1String("shortcut") && root.parentRootId == id) {
            delete m_roots.take(root.id);
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

QJsonObject Daemon::status() const
{
    const AccountInfo account = Config::account();
    const GeneralConfig general = Config::general();
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
        roots.append(QJsonObject::fromVariantMap(controller->status()));
    }
    return QJsonObject{
        {QStringLiteral("version"), QCoreApplication::applicationVersion()},
        {QStringLiteral("signedIn"), m_signedIn},
        {QStringLiteral("setupDone"), general.setupDone},
        {QStringLiteral("baseFolder"), general.baseFolder},
        {QStringLiteral("pauseOnMeteredNetwork"), general.pauseOnMeteredNetwork},
        {QStringLiteral("account"),
         QJsonObject{{QStringLiteral("displayName"), account.displayName},
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
        createController(root);
        changed = true;
    }
    for (auto it = existing.constBegin(); it != existing.constEnd(); ++it) {
        if (!wanted.contains(it.key())) {
            delete m_roots.take(it.key());
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
        const QString organization = Config::account().organization;
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

void Daemon::notifySignedOut()
{
    if (m_signedOutNotified || !notificationsAvailable()) {
        return;
    }
    m_signedOutNotified = true;
    auto *notification = new KNotification(QStringLiteral("signedOut"), KNotification::Persistent);
    notification->setComponentName(componentName);
    notification->setTitle(i18n("Sign in again"));
    notification->setText(i18n("Your session expired, so files are not syncing."));
    KNotificationAction *signIn = notification->addAction(i18n("Sign In"));
    connect(signIn, &KNotificationAction::activated, this, [] {
        launchApp({QStringLiteral("--sign-in")});
    });
    notification->sendEvent();
}

}
