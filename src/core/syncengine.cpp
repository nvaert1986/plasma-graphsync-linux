// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "syncengine.h"
#include "autofreeup.h"
#include "placeholder.h"
#include "ratelimiter.h"
#include "auth.h"
#include "foldericon.h"
#include "graphclient.h"
#include "ignorerules.h"
#include "graphsync_debug.h"
#include "quickxorhash.h"

#include <KLocalizedString>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMimeDatabase>
#include <QScopeGuard>
#include <QStorageInfo>
#include <QSysInfo>
#include <QUrl>

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <limits>
#include <cstdio>

namespace GraphSync
{

namespace
{
constexpr qint64 simpleUploadLimit = 4 * 1024 * 1024;
constexpr qint64 fragmentSize = 320 * 1024 * 32; // 10 MiB; fragments must be multiples of 320 KiB
constexpr int maxActivity = 200;
constexpr int transferConcurrency = 8;
constexpr int transferBatch = 64; // bounds the number of open temp files
constexpr int commitEvery = 200;
// Local deletes propagated in one cycle above this share of known items are refused
constexpr double bigDeleteShare = 0.3;
constexpr int bigDeleteMinimum = 50;
// uploadContent's status when the local file cannot be read (unlike 0: no connection)
constexpr int localReadError = -1;
// Automatic free up space runs at most this often per location
constexpr qint64 autoFreeUpEverySecs = 6 * 3600;
constexpr int maxItemAttempts = 5;

QString joinRel(const QString &parent, const QString &name)
{
    return parent.isEmpty() ? name : parent + QLatin1Char('/') + name;
}

QString parentOf(const QString &rel)
{
    const int slash = rel.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : rel.left(slash);
}

QString nameOf(const QString &rel)
{
    return rel.mid(rel.lastIndexOf(QLatin1Char('/')) + 1);
}

int depthOf(const QString &rel)
{
    return rel.isEmpty() ? 0 : static_cast<int>(rel.count(QLatin1Char('/'))) + 1;
}

QString encodeSegment(const QString &name)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(name));
}

}

SyncEngine::SyncEngine(const RootConfig &root, Auth *auth, QObject *parent)
    : QObject(parent)
    , m_root(root)
    , m_auth(auth)
{
    m_status = {{QStringLiteral("id"), root.id},
                {QStringLiteral("name"), root.name},
                {QStringLiteral("localPath"), root.localPath},
                {QStringLiteral("state"), QStringLiteral("idle")}};
}

SyncEngine::~SyncEngine() = default;

void SyncEngine::setMode(Mode mode)
{
    m_mode = mode;
}

// ---------------------------------------------------------------------------------------
// Control, status and logging

void SyncEngine::requestSync()
{
    if (m_running) {
        m_pending = true;
        return;
    }
    do {
        m_pending = false;
        syncOnce();
    } while (m_pending && !m_paused);
}

void SyncEngine::setPaused(bool paused)
{
    m_paused = paused;
    m_cancel = paused;
    setState(paused ? QStringLiteral("paused") : QStringLiteral("idle"));
    if (!paused) {
        QMetaObject::invokeMethod(this, &SyncEngine::requestSync, Qt::QueuedConnection);
    }
}

void SyncEngine::invalidatePushEndpoint()
{
    m_pushExpires = {};
}

void SyncEngine::setExcludedPaths(const QStringList &paths)
{
    m_root.excludedPaths = paths;
    m_excludedDirty = true;
    // A folder that is synced again must be fetched completely: start over with a full delta.
    // Also when this runs inside a cycle (its network waits run queued calls): that cycle must
    // not store its delta position over this afterwards.
    if (m_db) {
        m_db->setMeta(QStringLiteral("deltaLink"), QString());
    }
    m_startOver = true;
    requestSync();
}

void SyncEngine::checkEverything()
{
    if (m_db) {
        m_db->setMeta(QStringLiteral("deltaLink"), QString());
        m_db->setMeta(QStringLiteral("retry"), QString());
    }
    m_startOver = true;
    {
        QMutexLocker locker(&m_statusMutex);
        m_status.remove(QStringLiteral("persistentErrors"));
    }
    log(i18n("Checking all files again"));
    requestSync();
}

void SyncEngine::resolveBigDelete(const QString &action)
{
    m_bigDeleteDecision = action == QLatin1String("delete") ? 1 : (action == QLatin1String("restore") ? 2 : 0);
    {
        QMutexLocker locker(&m_statusMutex);
        m_status.remove(QStringLiteral("pendingBigDelete"));
    }
    requestSync();
}

bool SyncEngine::isExcluded(const QString &rel) const
{
    for (const QString &excluded : m_root.excludedPaths) {
        if (rel == excluded || rel.startsWith(excluded + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

void SyncEngine::purgeExcluded()
{
    QList<QPair<int, QString>> ordered;
    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        const auto rel = relPath(it.key());
        if (rel && isExcluded(*rel)) {
            ordered.append({depthOf(*rel), it.key()});
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });
    m_db->begin();
    DeleteContext context = deleteContext();
    for (const auto &[depth, id] : std::as_const(ordered)) {
        Q_UNUSED(depth)
        if (m_items.contains(id)) {
            deleteLocalSubtree(id, true, context, false);
        }
    }
    m_db->commit();
}

void SyncEngine::fetchQuota()
{
    const GraphResponse response = m_graph->get(QStringLiteral("/drives/%1?$select=quota").arg(m_root.driveId));
    if (!response.ok()) {
        return;
    }
    const QJsonObject quota = response.json().value(QLatin1String("quota")).toObject();
    QMutexLocker locker(&m_statusMutex);
    m_status.insert(QStringLiteral("quotaUsed"), quota.value(QLatin1String("used")).toDouble());
    m_status.insert(QStringLiteral("quotaTotal"), quota.value(QLatin1String("total")).toDouble());
}

QList<DriveItem> SyncEngine::fetchRetryItems(QHash<QString, int> *attempts, QStringList *unreachable)
{
    QList<DriveItem> items;
    const QJsonObject stored = QJsonDocument::fromJson(m_db->meta(QStringLiteral("retry")).toUtf8()).object();
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        attempts->insert(it.key(), it.value().toInt());
        const GraphResponse response = m_graph->get(itemPath(it.key()) + QStringLiteral("?$select=") + DriveItem::selectFields());
        if (response.status == 404) {
            DriveItem gone;
            gone.id = it.key();
            gone.deleted = true;
            items.append(gone);
        } else if (response.ok()) {
            items.append(DriveItem::fromJson(response.json()));
        } else {
            // Not reachable now (server error, no connection): stays on the list, never forgotten
            unreachable->append(it.key());
        }
    }
    return items;
}

QVariantMap SyncEngine::status() const
{
    QMutexLocker locker(&m_statusMutex);
    return m_status;
}

QVariantList SyncEngine::recentActivity() const
{
    QMutexLocker locker(&m_statusMutex);
    return m_activity;
}

SyncEngine::Stats SyncEngine::lastStats() const
{
    QMutexLocker locker(&m_statusMutex);
    return m_lastStats;
}

void SyncEngine::log(const QString &message, const QString &action, const QString &rel, const QString &by)
{
    // Errors go to the journal once, as warnings (recordError comes through here)
    if (action == QLatin1String("error")) {
        qCWarning(GRAPHSYNC).noquote() << QStringLiteral("[%1]").arg(m_root.name) << message;
    } else {
        qCInfo(GRAPHSYNC).noquote() << QStringLiteral("[%1]").arg(m_root.name) << message;
    }
    QVariantMap entry{{QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                      {QStringLiteral("message"), message},
                      {QStringLiteral("action"), action},
                      {QStringLiteral("root"), m_root.name},
                      {QStringLiteral("rootId"), m_root.id}};
    if (!by.isEmpty()) {
        // Changed online by someone else ("Updated by ...")
        entry.insert(QStringLiteral("by"), by);
    }
    if (!rel.isEmpty()) {
        const QString path = absPath(rel);
        entry.insert(QStringLiteral("path"), path);
        entry.insert(QStringLiteral("name"), nameOf(rel));
        entry.insert(QStringLiteral("folder"), parentOf(rel));
        static const QMimeDatabase mimeDb;
        entry.insert(QStringLiteral("icon"), mimeDb.mimeTypeForFile(path, QMimeDatabase::MatchExtension).iconName());
    }
    {
        QMutexLocker locker(&m_statusMutex);
        m_activity.prepend(entry);
        while (m_activity.size() > maxActivity) {
            m_activity.removeLast();
        }
    }
    Q_EMIT activity(message);
    Q_EMIT activityEntry(entry);
}

void SyncEngine::markError(const QString &rel)
{
    m_errorPaths.insert(rel);
}

void SyncEngine::setBusy(const QStringList &rels)
{
    m_busyPaths = QSet<QString>(rels.cbegin(), rels.cend());
    publishSnapshot(false);
}

void SyncEngine::publishSnapshot(bool rebuildKnown)
{
    if (rebuildKnown) {
        m_knownCache.clear();
        m_dehydratedCache.clear();
        m_sharedCache.clear();
        m_knownCache.reserve(m_items.size());
        for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
            if (const auto rel = relPath(it.key())) {
                m_knownCache.insert(*rel, {it->localSize, it->localMtimeNs, it->isFolder});
                if (it->dehydrated) {
                    m_dehydratedCache.insert(*rel, it.key());
                }
                if (it->shared) {
                    m_sharedCache.insert(*rel);
                }
            }
        }
    }
    auto snapshot = std::make_shared<StatusSnapshot>();
    snapshot->known = m_knownCache;
    snapshot->busy = m_busyPaths;
    snapshot->errors = m_errorPaths;
    snapshot->excluded = m_root.excludedPaths;
    snapshot->userRules = m_userRules;
    snapshot->dehydrated = m_dehydratedCache;
    snapshot->shared = m_sharedCache;
    {
        QMutexLocker locker(&m_statusMutex);
        m_snapshot = std::move(snapshot);
    }
    Q_EMIT snapshotChanged();
}

std::shared_ptr<const SyncEngine::StatusSnapshot> SyncEngine::statusSnapshot() const
{
    QMutexLocker locker(&m_statusMutex);
    return m_snapshot;
}

void SyncEngine::recordError(const QString &message)
{
    m_cycleErrors.append(message);
    log(message, QStringLiteral("error"));
}

void SyncEngine::uploadFailed(const QString &rel, int status)
{
    markError(rel);
    if (status == localReadError) {
        // Only this file: the other uploads go on
        recordError(i18n("Cannot read %1 here to upload it; check its permissions", rel));
        return;
    }
    if (status != 0 || m_paused) {
        recordError(i18n("Upload of %1 failed (HTTP %2)", rel, status));
        return;
    }
    // The connection dropped during the cycle: the remaining uploads wait for the next attempt,
    // and this is shown as being offline, not as a sync problem
    const QString message = i18n("No connection to Microsoft 365; syncing continues when the network is back.");
    if (!m_offline) {
        qCInfo(GRAPHSYNC).noquote() << QStringLiteral("[%1]").arg(m_root.name) << "connection lost while uploading" << rel;
        m_offline = true;
    }
    if (!m_cycleErrors.contains(message)) {
        m_cycleErrors.append(message);
    }
}

void SyncEngine::setState(const QString &state)
{
    QVariantMap snapshot;
    {
        QMutexLocker locker(&m_statusMutex);
        m_status.insert(QStringLiteral("state"), state);
        snapshot = m_status;
    }
    Q_EMIT statusChanged(snapshot);
}

// ---------------------------------------------------------------------------------------
// Cycle

bool SyncEngine::ensureInitialized(QString *error)
{
    if (m_graph) {
        return true;
    }
    m_graph = std::make_unique<GraphClient>(m_auth);
    m_graph->setCancelFlag(&m_cancel);
    m_graph->setTransferObserver([this](qint64 bytes) {
        m_transfer.bytesDone += bytes;
        publishTransfer(false);
    });
    m_db = std::make_unique<StateDb>(m_root.id);
    if (!m_db->open(error)) {
        m_graph.reset();
        m_db.reset();
        return false;
    }
    removeStaleTempFiles();
    return true;
}

bool SyncEngine::checkLocalRoot(QString *error)
{
    const QFileInfo info(m_root.localPath);
    // Desktop, Documents and Pictures (PC folder backup) keep their own icons; the emblem shows
    // that they are synced
    const bool ownIcon = m_root.backup.isEmpty();
    if (info.isDir()) {
        if (ownIcon) {
            FolderIcon::apply(m_root.localPath, QStringLiteral("folder-cloud"));
        }
        return true;
    }
    if (info.exists()) {
        *error = i18n("%1 exists but is not a folder", m_root.localPath);
        return false;
    }
    // A missing root after an earlier sync usually means an unmounted disk or a moved folder.
    // Recreating it empty would look like "the user deleted everything".
    if (!m_db->loadItems().isEmpty()) {
        *error = i18n("Local folder %1 is missing; not syncing (re-create it or remove the sync root)", m_root.localPath);
        return false;
    }
    if (!QDir().mkpath(m_root.localPath)) {
        *error = i18n("Cannot create %1", m_root.localPath);
        return false;
    }
    if (ownIcon) {
        FolderIcon::apply(m_root.localPath, QStringLiteral("folder-cloud"));
    }
    return true;
}

void SyncEngine::removeStaleTempFiles()
{
    QDirIterator it(m_root.localPath,
                    {IgnoreRules::tempPrefix + QLatin1Char('*'), IgnoreRules::legacyTempPrefix + QLatin1Char('*')},
                    QDir::Files | QDir::Hidden,
                    QDirIterator::Subdirectories);
    // Only old ones: a shortcut folder inside this one is synced by its own engine, which may be
    // downloading right now (a recent leftover is overwritten by the next download of that item)
    const QDateTime staleBefore = QDateTime::currentDateTimeUtc().addSecs(-10 * 60);
    while (it.hasNext()) {
        const QString path = it.next();
        if (QFileInfo(path).lastModified().toUTC() < staleBefore) {
            QFile::remove(path);
        }
    }
}

bool SyncEngine::syncOnce(QString *errorOut)
{
    QString error;
    if (m_paused) {
        return true;
    }
    m_running = true;
    m_cancel = false;
    m_cycleErrors.clear();
    m_stats = {};
    // Settings are read per cycle, so a change in the Settings window applies to the next one
    m_settings = Config::general();
    m_userRules = IgnoreRules::UserRules(m_settings.excludePatterns, m_settings.skipDotFiles);
    m_noSyncCache.clear();
    // "Updated by <name>" leaves out our own changes: the user of this root's account
    m_selfId = m_auth->config().objectId;
    RateLimiter::setLimits(qint64(m_settings.downloadLimitKBps) * 1024, qint64(m_settings.uploadLimitKBps) * 1024);
    m_lowSpace = false;
    m_lowSpaceMessage.clear();
    m_offline = false;
    m_transfer = {};
    {
        // A removed shortcut waits for a whole cycle that started after its removal (RootController::drain)
        QMutexLocker locker(&m_statusMutex);
        m_status.insert(QStringLiteral("cycleStarted"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    }
    setState(QStringLiteral("syncing"));

    bool ok = ensureInitialized(&error) && checkLocalRoot(&error);
    if (ok) {
        m_items = m_db->loadItems();
        m_rootItemId = m_root.remoteRootId.isEmpty() ? m_db->meta(QStringLiteral("rootItemId")) : m_root.remoteRootId;
        loadShortcuts();
        // Files On-Demand can be switched per root in the Settings window
        const QList<RootConfig> roots = Config::roots();
        for (const RootConfig &root : roots) {
            if (root.id == m_root.id) {
                m_root.filesOnDemand = root.filesOnDemand;
            }
        }
        m_pins = m_db->loadPins();
        m_hasDehydrated = std::any_of(m_items.cbegin(), m_items.cend(), [](const ItemRow &row) {
            return row.dehydrated;
        });
        if (m_excludedDirty) {
            purgeExcluded();
            m_excludedDirty = false;
        }
        m_errorPaths.clear();
        m_busyPaths.clear();
        publishSnapshot(true);
        ok = remotePhase(&error);
        if (ok && m_mode == Mode::TwoWay && !m_paused) {
            ok = localPhase(&error);
        } else if (m_lowSpace && m_mode == Mode::TwoWay && !m_paused && !m_cancel) {
            // No room for downloads: local changes are still uploaded (the skipped downloads have
            // no state yet, and the delta position stays, so they come once there is room)
            QString localError;
            if (!localPhase(&localError)) {
                recordError(localError);
            }
        }
        if (m_forceResync) {
            m_db->setMeta(QStringLiteral("deltaLink"), QString());
            m_forceResync = false;
        }
        if (ok && !m_paused) {
            refreshPushEndpoint();
            if (m_root.kind == QLatin1String("personal")) {
                fetchQuota();
            }
        }
    }
    if (!ok) {
        if (m_offline) {
            // Shown as the state, not added to the activity list on every attempt
            m_cycleErrors.append(error);
        } else {
            recordError(error);
        }
    }

    {
        QMutexLocker locker(&m_statusMutex);
        m_lastStats = m_stats;
        m_status.insert(QStringLiteral("lastSync"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        m_status.insert(QStringLiteral("lastErrors"), m_cycleErrors);
        m_status.insert(QStringLiteral("items"), static_cast<int>(m_items.size()));
        m_status.insert(QStringLiteral("downloaded"), m_stats.downloaded);
        m_status.insert(QStringLiteral("uploaded"), m_stats.uploaded);
        m_status.insert(QStringLiteral("conflicts"), m_stats.conflicts);
        m_status.insert(QStringLiteral("kind"), m_root.kind);
        m_status.insert(QStringLiteral("webUrl"), m_root.webUrl);
        m_status.insert(QStringLiteral("excludedPaths"), m_root.excludedPaths);
        m_status.insert(QStringLiteral("parentRootId"), m_root.parentRootId);
        m_status.insert(QStringLiteral("filesOnDemand"), m_root.filesOnDemand);
        m_status.insert(QStringLiteral("onlineOnly"), static_cast<int>(std::count_if(m_items.cbegin(), m_items.cend(), [](const ItemRow &row) {
                            return row.dehydrated;
                        })));
    }
    m_busyPaths.clear();
    {
        QMutexLocker locker(&m_statusMutex);
        m_status.remove(QStringLiteral("transfer"));
    }
    if (m_db) {
        publishSnapshot(true);
        emitShortcuts();
    }
    m_running = false;
    // Requests that arrived during the cycle outside its safe points
    QMetaObject::invokeMethod(this, &SyncEngine::processRequests, Qt::QueuedConnection);
    setState(m_paused ? QStringLiteral("paused")
                      : (m_offline ? QStringLiteral("offline") : (m_cycleErrors.isEmpty() ? QStringLiteral("idle") : QStringLiteral("error"))));
    if (errorOut) {
        *errorOut = m_cycleErrors.join(QLatin1Char('\n'));
    }
    return ok;
}

// ---------------------------------------------------------------------------------------
// Files On-Demand

StateDb::Pin SyncEngine::effectivePin(const QString &rel, bool *explicitPin) const
{
    *explicitPin = false;
    if (!m_root.filesOnDemand) {
        return StateDb::Pin::AlwaysKeep;
    }
    // The nearest choice up the tree; without one, new files stay online
    QString path = rel;
    while (true) {
        const auto it = m_pins.constFind(path);
        if (it != m_pins.constEnd()) {
            *explicitPin = true;
            return *it;
        }
        if (path.isEmpty()) {
            break;
        }
        path = parentOf(path);
    }
    return StateDb::Pin::OnlineOnly;
}

bool SyncEngine::createPlaceholder(const DriveItem &item, const QString &rel)
{
    QString error;
    if (!Placeholder::write(absPath(rel), item.size, item.modified, &error)) {
        markError(rel);
        recordError(i18n("Cannot write the placeholder for %1: %2", rel, error));
        return false;
    }
    ItemRow row;
    row.id = item.id;
    row.parentId = item.parentId;
    row.name = item.name;
    row.remoteETag = item.eTag;
    row.remoteCTag = item.cTag;
    row.remoteHash = item.quickXorHash;
    row.remoteSize = item.size;
    row.dehydrated = true;
    if (const auto placeholder = LocalScanner::stat(absPath(rel) + Placeholder::suffix)) {
        row.localInode = placeholder->inode;
    }
    m_items.insert(row.id, row);
    saveRow(row);
    m_hasDehydrated = true;
    return true;
}

bool SyncEngine::dehydrate(const QString &id, const QString &rel)
{
    ItemRow &row = m_items[id];
    const QString abs = absPath(rel);
    const auto local = LocalScanner::stat(abs);
    if (!local || local->isFolder) {
        return false;
    }
    // Only what is in sync: local changes are uploaded first, then freed up in a later cycle
    if (localChangedSinceBaseline(row, *local, rel)) {
        return false;
    }
    QString error;
    if (!Placeholder::write(abs, row.remoteSize, QFileInfo(abs).lastModified(), &error)) {
        markError(rel);
        recordError(i18n("Cannot write the placeholder for %1: %2", rel, error));
        return false;
    }
    // Moved aside first (atomic), then checked to be the very file that was found unchanged: a
    // save in between (new content, or a new file in its place) is put back untouched
    const QString aside = abs + QStringLiteral(".graphsync-freeing");
    if (::rename(QFile::encodeName(abs).constData(), QFile::encodeName(aside).constData()) != 0) {
        QFile::remove(abs + Placeholder::suffix);
        markError(rel);
        recordError(i18n("Cannot free up the space of %1", rel));
        return false;
    }
    const auto moved = LocalScanner::stat(aside);
    if (!moved || moved->inode != local->inode || moved->size != local->size || moved->mtimeNs != local->mtimeNs) {
        ::rename(QFile::encodeName(aside).constData(), QFile::encodeName(abs).constData());
        QFile::remove(abs + Placeholder::suffix);
        return false;
    }
    QFile::remove(aside);
    row.dehydrated = true;
    row.localSize = -1;
    row.localMtimeNs = 0;
    if (const auto placeholder = LocalScanner::stat(abs + Placeholder::suffix)) {
        row.localInode = placeholder->inode;
    }
    saveRow(row);
    m_hasDehydrated = true;
    return true;
}

void SyncEngine::followPins(const QString &fromRel, const QString &toRel)
{
    if (m_pins.isEmpty()) {
        return;
    }
    // Pins are kept on the real path; an online-only file moves as its placeholder
    const auto real = [](const QString &rel) {
        return Placeholder::hasSuffix(rel) ? rel.chopped(Placeholder::suffix.size()) : rel;
    };
    m_db->movePins(real(fromRel), real(toRel));
    m_pins = m_db->loadPins();
}

void SyncEngine::pruneStalePins()
{
    if (m_pins.isEmpty()) {
        return;
    }
    // A pin whose file or folder is gone (deleted here or online) would otherwise stay forever.
    // Gone means: not on disk, as itself or as a placeholder. A path that exists only here
    // (pinned before its first upload) keeps its pin
    QStringList stale;
    for (auto it = m_pins.constBegin(); it != m_pins.constEnd(); ++it) {
        if (it.key().isEmpty()) {
            continue;
        }
        const QString abs = absPath(it.key());
        if (!LocalScanner::stat(abs) && !LocalScanner::stat(abs + Placeholder::suffix)) {
            stale.append(it.key());
        }
    }
    if (!stale.isEmpty()) {
        qCDebug(GRAPHSYNC) << "removing pins of paths that no longer exist" << stale;
        m_db->removePins(stale);
        m_pins = m_db->loadPins();
    }
}

void SyncEngine::applyPins()
{
    pruneStalePins();
    // Nothing to enforce: no choices made and nothing online-only
    if (m_pins.isEmpty() && !m_hasDehydrated) {
        return;
    }
    int freed = 0;
    // Files programs have open, read once when needed
    std::optional<QSet<QString>> open;
    // Already queued by the delta (an online-only file that changed): never download it twice
    QSet<QString> queued;
    for (const QueuedDownload &entry : std::as_const(m_downloadQueue)) {
        queued.insert(entry.item.id);
    }
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (it->isFolder || queued.contains(it.key())) {
            continue;
        }
        const auto rel = relPath(it.key());
        if (!rel || isExcluded(*rel) || isUnderShortcut(*rel) || isLeftAlone(*rel)) {
            continue;
        }
        bool explicitPin = false;
        const StateDb::Pin pin = effectivePin(*rel, &explicitPin);
        if (it->dehydrated && pin == StateDb::Pin::AlwaysKeep) {
            // Always keep (or Files On-Demand switched off): download it
            DriveItem item;
            item.id = it->id;
            item.parentId = it->parentId;
            item.name = it->name;
            item.size = it->remoteSize;
            item.quickXorHash = it->remoteHash;
            item.eTag = it->remoteETag;
            item.cTag = it->remoteCTag;
            queueDownload(item, *rel);
        } else if (!it->dehydrated && pin == StateDb::Pin::OnlineOnly && explicitPin) {
            // Free up space was chosen for it (only an explicit choice frees up files that are here).
            // Not while a program has it open: saving would bring it back as a local change. The
            // pin stays, so a later cycle frees it.
            const QString abs = absPath(*rel);
            if (AutoFreeUp::hasOfficeLock(abs)) {
                continue;
            }
            if (!open) {
                open = AutoFreeUp::openFiles();
            }
            if (open->contains(abs) || open->contains(QFileInfo(abs).canonicalFilePath())) {
                continue;
            }
            if (dehydrate(it.key(), *rel)) {
                ++freed;
            }
        }
    }
    if (freed > 0) {
        log(i18np("Freed up the space of 1 file", "Freed up the space of %1 files", freed));
    }
}

void SyncEngine::noteOpened(const QString &rel)
{
    QMutexLocker locker(&m_requestMutex);
    m_opened.insert(rel, QDateTime::currentSecsSinceEpoch());
}

void SyncEngine::autoFreeUp()
{
    // What the file watcher saw since the last cycle
    QHash<QString, qint64> seen;
    {
        QMutexLocker locker(&m_requestMutex);
        seen = std::exchange(m_opened, {});
    }
    // Opens are only kept while the feature is on for this location. Switched on, recording starts
    // anew (files opened while it was off must not look unused); switched off, the records go.
    const bool active = m_root.filesOnDemand && m_settings.freeUpAfterDays > 0;
    const QString recording = m_db->meta(QStringLiteral("openedRecording"));
    if (!active) {
        if (recording != QLatin1String("0")) {
            m_db->clearOpened();
            m_db->setMeta(QStringLiteral("openedRecording"), QStringLiteral("0"));
        }
        return;
    }
    if (recording != QLatin1String("1")) {
        m_db->setMeta(QStringLiteral("openedSince"), QString::number(QDateTime::currentSecsSinceEpoch()));
        m_db->setMeta(QStringLiteral("openedRecording"), QStringLiteral("1"));
    }
    if (!seen.isEmpty()) {
        m_db->noteOpened(seen);
    }
    if (m_paused || m_cancel) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime last = QDateTime::fromString(m_db->meta(QStringLiteral("lastAutoFreeUp")), Qt::ISODate);
    if (last.isValid() && last <= now && last.secsTo(now) < autoFreeUpEverySecs) {
        return;
    }
    m_db->setMeta(QStringLiteral("lastAutoFreeUp"), now.toString(Qt::ISODate));
    const bool accessTimes = AutoFreeUp::accessTimesKept(m_root.localPath);
    qint64 nowSecs = now.toSecsSinceEpoch();
#ifdef GRAPHSYNC_TESTING
    // Tests only (Debug builds of this project; never in a package): pretend it is this many days later
    nowSecs += qEnvironmentVariableIntValue("GRAPHSYNC_TEST_FREEUP_DAYS_LATER") * qint64(86400);
#endif
    const qint64 cutoff = nowSecs - qint64(m_settings.freeUpAfterDays) * 86400;
    // Opens are known since then; before it, every file counts as used then
    const qint64 openedSince = m_db->meta(QStringLiteral("openedSince")).toLongLong();
    const QHash<QString, qint64> opened = m_db->loadOpened();
    rebuildPathIndex();
    std::optional<QSet<QString>> open;
    QStringList freedRels;
    int freed = 0;
    // Changed online and about to be downloaded: freeing it now would be undone right away
    QSet<QString> queued;
    for (const QueuedDownload &entry : std::as_const(m_downloadQueue)) {
        queued.insert(entry.item.id);
    }
    qint64 bytes = 0;
    for (auto it = m_items.begin(); it != m_items.end() && !m_cancel; ++it) {
        if (it->isFolder || it->dehydrated || m_failedIds.contains(it.key()) || queued.contains(it.key())) {
            continue;
        }
        const auto rel = relPath(it.key());
        if (!rel || isExcluded(*rel) || isUnderShortcut(*rel) || isLeftAlone(*rel)) {
            continue;
        }
        // Never what the user chose to keep here
        bool explicitPin = false;
        const StateDb::Pin pin = effectivePin(*rel, &explicitPin);
        if (pin == StateDb::Pin::AlwaysKeep) {
            continue;
        }
        const QString abs = absPath(*rel);
        const auto used = AutoFreeUp::lastUsed(abs, accessTimes);
        if (!used || std::max({*used, openedSince, opened.value(*rel)}) > cutoff || AutoFreeUp::hasOfficeLock(abs)) {
            continue;
        }
        if (!open) {
            open = AutoFreeUp::openFiles();
        }
        // /proc lists canonical paths; the location may sit behind a symlink
        if (open->contains(abs) || open->contains(QFileInfo(abs).canonicalFilePath())) {
            continue;
        }
        const qint64 size = it->localSize;
        // dehydrate() only frees what is in sync (local changes are uploaded first)
        if (!dehydrate(it.key(), *rel)) {
            continue;
        }
        ++freed;
        bytes += std::max<qint64>(0, size);
        // "Available" (opened earlier) on the file itself has done its work
        if (m_pins.value(*rel) == StateDb::Pin::Available) {
            m_db->removePins({*rel});
            m_pins.remove(*rel);
        }
        freedRels.append(*rel);
    }
    // Records of freed files and of paths no longer known
    for (auto it = opened.constBegin(); it != opened.constEnd(); ++it) {
        if (!m_pathToId.contains(it.key())) {
            freedRels.append(it.key());
        }
    }
    m_db->removeOpened(freedRels);
    if (freed > 0) {
        log(i18np("Freed up %2 of 1 file not opened for %3 days",
                  "Freed up %2 of %1 files not opened for %3 days",
                  freed,
                  QLocale().formattedDataSize(bytes),
                  m_settings.freeUpAfterDays));
    }
}

void SyncEngine::requestHydrate(const QString &rel)
{
    {
        QMutexLocker locker(&m_requestMutex);
        m_hydrateRequests.append(rel);
    }
    QMetaObject::invokeMethod(this, &SyncEngine::processRequests, Qt::QueuedConnection);
}

void SyncEngine::requestPin(const QString &rel, StateDb::Pin pin)
{
    {
        QMutexLocker locker(&m_requestMutex);
        m_pinRequests.append({rel, pin});
    }
    QMetaObject::invokeMethod(this, &SyncEngine::processRequests, Qt::QueuedConnection);
}

void SyncEngine::requestSharingCheck(const QString &rel)
{
    {
        QMutexLocker locker(&m_requestMutex);
        m_sharingRequests.append(rel);
    }
    QMetaObject::invokeMethod(this, &SyncEngine::processRequests, Qt::QueuedConnection);
}

void SyncEngine::processRequests()
{
    // During a cycle only between two batches of downloads (runQueuedDownloads): a network wait
    // of the cycle also runs queued calls, and a download started there would write the same
    // temporary file as the batch. The requests wait; the cycle's end takes them.
    if (m_running && !m_atSafePoint) {
        return;
    }
    QStringList hydrate;
    QList<QPair<QString, StateDb::Pin>> pins;
    QStringList sharing;
    {
        QMutexLocker locker(&m_requestMutex);
        hydrate = std::exchange(m_hydrateRequests, {});
        pins = std::exchange(m_pinRequests, {});
        sharing = std::exchange(m_sharingRequests, {});
    }
    if (hydrate.isEmpty() && pins.isEmpty() && sharing.isEmpty()) {
        return;
    }
    QString error;
    if (!ensureInitialized(&error)) {
        for (const QString &rel : std::as_const(hydrate)) {
            Q_EMIT hydrated(rel, false, error);
        }
        return;
    }
    // Between cycles the state is loaded here; during a cycle it is current already. While idle
    // requests run, the engine counts as running, so a sync asked for meanwhile waits its turn
    // instead of starting inside this one's network calls.
    const bool idle = !m_running;
    if (idle) {
        m_running = true;
        m_items = m_db->loadItems();
        m_rootItemId = m_root.remoteRootId.isEmpty() ? m_db->meta(QStringLiteral("rootItemId")) : m_root.remoteRootId;
        m_db->begin();
    }
    for (const auto &[rel, pin] : std::as_const(pins)) {
        m_db->setPin(rel, pin);
    }
    if (!pins.isEmpty()) {
        m_pins = m_db->loadPins();
    }
    // Opening a file is the user's explicit wish: also while syncing is paused (the pause is for
    // syncing in the background). Pausing during the download still stops it.
    const bool liftPause = idle && m_paused && !hydrate.isEmpty();
    if (liftPause) {
        m_cancel = false;
    }
    for (const QString &rel : std::as_const(hydrate)) {
        QString hydrateError;
        const bool ok = hydrateNow(rel, &hydrateError);
        Q_EMIT hydrated(rel, ok, hydrateError);
    }
    if (liftPause && m_paused) {
        m_cancel = true;
    }
    sharing.removeDuplicates();
    for (const QString &rel : std::as_const(sharing)) {
        checkSharingNow(rel);
    }
    if (idle) {
        m_running = false;
        m_db->commit();
        m_transfer = {};
        {
            QMutexLocker locker(&m_statusMutex);
            m_status.remove(QStringLiteral("transfer"));
        }
        publishSnapshot(true);
    }
    if (!pins.isEmpty() || (idle && m_pending)) {
        // Downloading or freeing up the files happens in the next cycle (also one that was
        // asked for while this ran)
        m_pending = false;
        QMetaObject::invokeMethod(this, &SyncEngine::requestSync, Qt::QueuedConnection);
    }
}

void SyncEngine::checkSharingNow(const QString &rel)
{
    rebuildPathIndex();
    const QString id = m_pathToId.value(rel);
    if (id.isEmpty() || id == m_rootItemId) {
        return;
    }
    // Sharing of its own = a permission that is not inherited from a folder above (a link, an
    // invitation, or SharePoint's broken inheritance); the same items the delta feed marks. The
    // owner's own permission alone is no sharing.
    bool shared = false;
    QString link = itemPath(id) + QStringLiteral("/permissions");
    while (!link.isEmpty() && !shared) {
        const GraphResponse response = m_graph->get(link);
        if (!response.ok()) {
            qCDebug(GRAPHSYNC) << "Sharing check of" << rel << "failed:" << response.describe();
            return;
        }
        const QJsonObject page = response.json();
        const QJsonArray permissions = page.value(QLatin1String("value")).toArray();
        shared = std::any_of(permissions.begin(), permissions.end(), [](const QJsonValue &value) {
            const QJsonObject permission = value.toObject();
            const QJsonArray roles = permission.value(QLatin1String("roles")).toArray();
            const bool ownerOnly = !roles.isEmpty() && std::all_of(roles.begin(), roles.end(), [](const QJsonValue &role) {
                return role.toString() == QLatin1String("owner");
            });
            return !permission.contains(QLatin1String("inheritedFrom")) && !ownerOnly;
        });
        link = page.value(QLatin1String("@odata.nextLink")).toString();
    }
    ItemRow &row = m_items[id];
    if (row.shared != shared) {
        row.shared = shared;
        saveRow(row);
    }
}

bool SyncEngine::hydrateNow(const QString &rel, QString *error)
{
    rebuildPathIndex();
    const QString id = m_pathToId.value(rel);
    if (id.isEmpty()) {
        *error = i18n("%1 is not known to the sync engine", rel);
        return false;
    }
    if (!m_items.value(id).dehydrated) {
        return QFileInfo::exists(absPath(rel));
    }
    // The current state online, so the download checks against the right hash
    const GraphResponse response = m_graph->get(itemPath(id) + QStringLiteral("?$select=") + DriveItem::selectFields());
    if (!response.ok()) {
        // Shown in a notification: say what happened, not the HTTP details
        if (response.status == 404) {
            *error = i18n("It was deleted or moved online; the next sync updates this folder.");
        } else if (response.status == 0) {
            *error = i18n("No connection to Microsoft 365. Check the network connection.");
        } else {
            *error = response.describe();
        }
        return false;
    }
    const qsizetype errorsBefore = m_cycleErrors.size();
    const bool ok = download(DriveItem::fromJson(response.json()), rel);
    if (!ok) {
        *error = m_cycleErrors.size() > errorsBefore ? m_cycleErrors.constLast() : i18n("The download failed");
        return false;
    }
    // Opened or downloaded on request: available on this device from now on, as on Windows. An
    // online-only choice on the file or a folder above would otherwise free it again at the next
    // cycle, possibly while a program has it open
    bool explicitPin = false;
    if (effectivePin(rel, &explicitPin) == StateDb::Pin::OnlineOnly && explicitPin) {
        m_db->setPin(rel, StateDb::Pin::Available);
        m_pins = m_db->loadPins();
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// User exclusions and transfer progress

bool SyncEngine::isLeftAlone(const QString &rel)
{
    // Names never synced in either direction: temp, lock and desktop files (the scanner leaves them
    // out, so one downloaded from online would look deleted here and be deleted online), our own
    // temp downloads, and *.cloud, the placeholder name
    const QStringList names = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (rel.endsWith(Placeholder::suffix) || std::any_of(names.cbegin(), names.cend(), [](const QString &name) {
            return IgnoreRules::isIgnored(name);
        })) {
        return true;
    }
    if (m_userRules.matchesPath(rel)) {
        return true;
    }
    // A folder holding a .nosync file is left out with everything in it
    QString prefix;
    for (const QString &part : names) {
        prefix = joinRel(prefix, part);
        auto it = m_noSyncCache.constFind(prefix);
        if (it == m_noSyncCache.constEnd()) {
            it = m_noSyncCache.insert(prefix, QFileInfo::exists(absPath(prefix) + QLatin1Char('/') + IgnoreRules::noSyncMarker));
        }
        if (*it) {
            return true;
        }
    }
    return false;
}

void SyncEngine::beginTransfers(int files, qint64 bytes)
{
    if (files <= 0) {
        return;
    }
    m_transfer.files += files;
    m_transfer.bytes += bytes;
    publishTransfer(true);
}

void SyncEngine::transferFinished()
{
    ++m_transfer.filesDone;
    publishTransfer(m_transfer.filesDone >= m_transfer.files);
}

void SyncEngine::publishTransfer(bool force)
{
    // At most once a second, plus at the start and the end of a batch
    if (!force && m_transferPublished.isValid() && m_transferPublished.elapsed() < 1000) {
        return;
    }
    m_transferPublished.start();
    QVariantMap snapshot;
    {
        QMutexLocker locker(&m_statusMutex);
        m_status.insert(QStringLiteral("transfer"),
                        QVariantMap{{QStringLiteral("files"), m_transfer.files},
                                    {QStringLiteral("filesDone"), std::min(m_transfer.filesDone, m_transfer.files)},
                                    {QStringLiteral("bytes"), m_transfer.bytes},
                                    {QStringLiteral("bytesDone"), std::min(m_transfer.bytesDone, m_transfer.bytes)}});
        snapshot = m_status;
    }
    Q_EMIT statusChanged(snapshot);
}

qint64 SyncEngine::fragmentSizeNow() const
{
    // With an upload limit, smaller pieces keep the rate even (multiples of 320 KiB)
    const qint64 limit = RateLimiter::uploadLimit();
    if (limit <= 0) {
        return fragmentSize;
    }
    constexpr qint64 unit = 320 * 1024;
    return std::clamp<qint64>((limit * 2 / unit) * unit, unit, fragmentSize);
}

// ---------------------------------------------------------------------------------------
// Shortcuts ("Add shortcut to My files")

void SyncEngine::loadShortcuts()
{
    m_shortcuts.clear();
    const QJsonObject stored = QJsonDocument::fromJson(m_db->meta(QStringLiteral("shortcuts")).toUtf8()).object();
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        const QJsonObject value = it.value().toObject();
        Shortcut shortcut;
        shortcut.id = it.key();
        shortcut.name = value.value(QLatin1String("name")).toString();
        shortcut.parentId = value.value(QLatin1String("parentId")).toString();
        shortcut.driveId = value.value(QLatin1String("driveId")).toString();
        shortcut.remoteId = value.value(QLatin1String("remoteId")).toString();
        shortcut.webUrl = value.value(QLatin1String("webUrl")).toString();
        shortcut.inode = value.value(QLatin1String("inode")).toString().toULongLong();
        m_shortcuts.insert(shortcut.id, shortcut);
    }
    m_detachedShortcuts.clear();
    const QJsonArray detached = QJsonDocument::fromJson(m_db->meta(QStringLiteral("detachedShortcuts")).toUtf8()).array();
    for (const QJsonValue &value : detached) {
        m_detachedShortcuts.append(value.toString());
    }
}

void SyncEngine::saveShortcuts()
{
    QJsonObject stored;
    for (const Shortcut &shortcut : std::as_const(m_shortcuts)) {
        stored.insert(shortcut.id,
                      QJsonObject{{QStringLiteral("name"), shortcut.name},
                                  {QStringLiteral("parentId"), shortcut.parentId},
                                  {QStringLiteral("driveId"), shortcut.driveId},
                                  {QStringLiteral("remoteId"), shortcut.remoteId},
                                  {QStringLiteral("webUrl"), shortcut.webUrl},
                                  {QStringLiteral("inode"), QString::number(shortcut.inode)}});
    }
    m_db->setMeta(QStringLiteral("shortcuts"), QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact)));
    m_db->setMeta(QStringLiteral("detachedShortcuts"),
                  QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(m_detachedShortcuts)).toJson(QJsonDocument::Compact)));
}

std::optional<QString> SyncEngine::shortcutRel(const Shortcut &shortcut) const
{
    const auto parentRel = shortcut.parentId == m_rootItemId ? std::optional<QString>(QString()) : relPath(shortcut.parentId);
    if (!parentRel) {
        return std::nullopt;
    }
    return joinRel(*parentRel, shortcut.name);
}

QSet<QString> SyncEngine::shortcutPaths() const
{
    QSet<QString> paths(m_detachedShortcuts.cbegin(), m_detachedShortcuts.cend());
    for (const Shortcut &shortcut : m_shortcuts) {
        if (const auto rel = shortcutRel(shortcut)) {
            paths.insert(*rel);
        }
    }
    return paths;
}

bool SyncEngine::isUnderShortcut(const QString &rel) const
{
    // Called for every path of a scan: most locations have no shortcuts at all
    if (m_shortcuts.isEmpty() && m_detachedShortcuts.isEmpty()) {
        return false;
    }
    const QSet<QString> paths = shortcutPaths();
    for (const QString &path : paths) {
        if (rel == path || rel.startsWith(path + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

void SyncEngine::applyRemoteShortcut(const DriveItem &item)
{
    const auto parentRel = item.parentId == m_rootItemId ? std::optional<QString>(QString()) : relPath(item.parentId);
    if (!parentRel) {
        // Outside this root (folder roots see the whole drive)
        if (m_shortcuts.contains(item.id)) {
            forgetShortcut(item.id, i18n("it was moved out of this folder"));
        }
        return;
    }
    const QString rel = joinRel(*parentRel, item.name);
    if (isExcluded(rel)) {
        if (m_shortcuts.contains(item.id)) {
            forgetShortcut(item.id, i18n("its folder is no longer synced"));
        }
        return;
    }

    auto existing = m_shortcuts.find(item.id);
    if (existing != m_shortcuts.end()) {
        // Renamed or moved online: move the local folder along (its own root follows on restart)
        const auto oldRel = shortcutRel(*existing);
        if (oldRel && *oldRel != rel && LocalScanner::stat(absPath(*oldRel))) {
            moveLocal(*oldRel, rel);
        }
        existing->name = item.name;
        existing->parentId = item.parentId;
        existing->driveId = item.remoteDriveId;
        existing->remoteId = item.remoteId;
        existing->webUrl = item.remoteWebUrl;
        return;
    }

    // New. Something already at that path would otherwise end up in the other library.
    if (LocalScanner::stat(absPath(rel)) && makeConflictCopy(rel).isEmpty()) {
        return;
    }
    Shortcut shortcut;
    shortcut.id = item.id;
    shortcut.name = item.name;
    shortcut.parentId = item.parentId;
    shortcut.driveId = item.remoteDriveId;
    shortcut.remoteId = item.remoteId;
    shortcut.webUrl = item.remoteWebUrl;
    m_shortcuts.insert(shortcut.id, shortcut);
    log(i18n("Shortcut %1 found; it is synced with the library it points to", rel), QStringLiteral("created"), rel);
}

void SyncEngine::forgetShortcut(const QString &id, const QString &reason)
{
    const auto it = m_shortcuts.constFind(id);
    if (it == m_shortcuts.constEnd()) {
        return;
    }
    if (const auto rel = shortcutRel(*it)) {
        if (LocalScanner::stat(absPath(*rel))) {
            // The daemon stops its root and moves the folder to the Trash; until then it is left alone
            m_detachedShortcuts.append(*rel);
        }
        log(i18n("Shortcut %1 is no longer synced: %2", *rel, reason), QStringLiteral("deleted"), *rel);
    }
    m_shortcuts.erase(it);
}

void SyncEngine::handleLocalShortcutChanges(const QHash<QString, LocalEntry> &scan)
{
    // Removed shortcuts whose folder is gone need no protection any more
    m_detachedShortcuts.removeIf([&scan](const QString &rel) {
        return !scan.contains(rel);
    });

    bool changed = false;
    const QStringList ids = m_shortcuts.keys();
    for (const QString &id : ids) {
        Shortcut &shortcut = m_shortcuts[id];
        const auto rel = shortcutRel(shortcut);
        if (!rel) {
            continue;
        }
        const auto here = scan.constFind(*rel);
        if (here != scan.constEnd() && here->isFolder) {
            if (shortcut.inode != here->inode) {
                shortcut.inode = here->inode;
                changed = true;
            }
            continue;
        }
        if (shortcut.inode == 0) {
            // Not created here yet; the shortcut's own root does that
            continue;
        }

        // Gone from its place: renamed or moved here (same folder, new path), or deleted
        QString newRel;
        for (auto it = scan.constBegin(); it != scan.constEnd(); ++it) {
            if (it->isFolder && it->inode == shortcut.inode && !m_pathToId.contains(it.key())) {
                newRel = it.key();
                break;
            }
        }
        if (!newRel.isEmpty()) {
            const QString parentRel = parentOf(newRel);
            const QString parentId = parentRel.isEmpty() ? m_rootItemId : m_pathToId.value(parentRel);
            if (parentId.isEmpty()) {
                recordError(i18n("Cannot move shortcut %1 to %2: the folder is not synced", *rel, newRel));
                continue;
            }
            QJsonObject body{{QStringLiteral("name"), nameOf(newRel)}};
            if (parentId != shortcut.parentId) {
                body.insert(QStringLiteral("parentReference"), QJsonObject{{QStringLiteral("id"), parentId}});
            }
            const GraphResponse response = m_graph->patchJson(itemPath(shortcut.id), body);
            if (!response.ok()) {
                recordError(i18n("Moving shortcut %1 to %2 online failed: %3", *rel, newRel, response.describe()));
                continue;
            }
            shortcut.name = nameOf(newRel);
            shortcut.parentId = parentId;
            changed = true;
            log(i18n("Moved shortcut %1 to %2 online", *rel, newRel), QStringLiteral("moved"), newRel);
            continue;
        }

        // Deleted here: like OneDrive, remove the shortcut (the library it points to is untouched)
        const GraphResponse response = m_graph->deleteResource(itemPath(shortcut.id));
        if (!response.ok() && response.status != 404) {
            recordError(i18n("Removing shortcut %1 online failed: %2", *rel, response.describe()));
            continue;
        }
        log(i18n("Removed shortcut %1 online: its folder was deleted here", *rel), QStringLiteral("deleted"), *rel);
        m_shortcuts.remove(id);
        changed = true;
    }
    if (changed) {
        saveShortcuts();
    }
}

void SyncEngine::emitShortcuts()
{
    QVariantList list;
    for (const Shortcut &shortcut : std::as_const(m_shortcuts)) {
        const auto rel = shortcutRel(shortcut);
        if (!rel) {
            continue;
        }
        list.append(QVariantMap{{QStringLiteral("id"), shortcut.id},
                                {QStringLiteral("name"), shortcut.name},
                                {QStringLiteral("rel"), *rel},
                                {QStringLiteral("driveId"), shortcut.driveId},
                                {QStringLiteral("remoteId"), shortcut.remoteId},
                                {QStringLiteral("webUrl"), shortcut.webUrl}});
    }
    Q_EMIT shortcutsChanged(list, m_detachedShortcuts);
}

// ---------------------------------------------------------------------------------------
// Paths

std::optional<QString> SyncEngine::relPath(const QString &id) const
{
    if (id == m_rootItemId) {
        return QString();
    }
    QStringList parts;
    QString current = id;
    for (int guard = 0; guard < 1000; ++guard) {
        const auto it = m_items.constFind(current);
        if (it == m_items.constEnd()) {
            return std::nullopt;
        }
        parts.prepend(it->name);
        if (it->parentId == m_rootItemId) {
            return parts.join(QLatin1Char('/'));
        }
        current = it->parentId;
    }
    return std::nullopt;
}

QString SyncEngine::absPath(const QString &rel) const
{
    return rel.isEmpty() ? m_root.localPath : m_root.localPath + QLatin1Char('/') + rel;
}

QString SyncEngine::itemPath(const QString &id) const
{
    return QStringLiteral("/drives/%1/items/%2").arg(m_root.driveId, id);
}

// Rows are written inside a transaction that is committed every few hundred changes, so an
// interrupted cycle keeps most of its progress
bool SyncEngine::remoteContentChanged(const ItemRow &row, const DriveItem &item)
{
    // Without a hash online (some SharePoint items) the content tag tells
    return item.quickXorHash.isEmpty() ? item.cTag != row.remoteCTag : (item.quickXorHash != row.remoteHash || item.size != row.remoteSize);
}

void SyncEngine::countWrite()
{
    if (++m_uncommitted >= commitEvery) {
        m_db->commit();
        m_db->begin();
        m_uncommitted = 0;
    }
}

void SyncEngine::saveRow(const ItemRow &row)
{
    m_db->upsert(row);
    countWrite();
}

void SyncEngine::dropRow(const QString &id)
{
    m_db->remove(id);
    countWrite();
}

bool SyncEngine::localChangedSinceBaseline(const ItemRow &row, const LocalEntry &entry, const QString &rel) const
{
    if (entry.size == row.localSize && entry.mtimeNs == row.localMtimeNs) {
        return false;
    }
    // A file that cannot be read counts as changed: it is never freed up or overwritten
    const QString hash = QuickXorHash::hashFile(absPath(rel));
    return hash.isEmpty() || hash != row.localHash;
}

void SyncEngine::storeLocalBaseline(ItemRow &row, const QString &rel, const QString &hash)
{
    if (const auto entry = LocalScanner::stat(absPath(rel))) {
        row.localSize = entry->size;
        row.localMtimeNs = entry->mtimeNs;
        row.localInode = entry->inode;
    }
    if (!row.isFolder) {
        row.localHash = hash;
    }
}

QString SyncEngine::knownItemAt(const QString &rel) const
{
    const QString name = nameOf(rel);
    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        if (it->name == name && relPath(it.key()) == rel) {
            return it.key();
        }
    }
    return {};
}

QString SyncEngine::makeConflictCopy(const QString &rel)
{
    const QString dir = parentOf(rel);
    const QString name = nameOf(rel);
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    const QString base = dot > 0 ? name.left(dot) : name;
    const QString ext = dot > 0 ? name.mid(dot) : QString();
    const QString host = QSysInfo::machineHostName();
    QString candidate;
    for (int n = 1;; ++n) {
        const QString suffix = n == 1 ? QStringLiteral("-%1").arg(host) : QStringLiteral("-%1-%2").arg(host).arg(n);
        // The copy's name must fit in a file name (255 bytes): a long base name is shortened,
        // never cut inside a character
        QString fitted = base;
        while (!fitted.isEmpty() && QFile::encodeName(fitted + suffix + ext).size() > 255) {
            fitted.chop(fitted.size() >= 2 && fitted.at(fitted.size() - 1).isLowSurrogate() ? 2 : 1);
        }
        candidate = joinRel(dir, fitted + suffix + ext);
        if (!QFileInfo::exists(absPath(candidate))) {
            break;
        }
    }
    if (::rename(QFile::encodeName(absPath(rel)).constData(), QFile::encodeName(absPath(candidate)).constData()) != 0) {
        markError(rel);
        recordError(i18n("Cannot rename %1 to keep a conflict copy", rel));
        return {};
    }
    ++m_stats.conflicts;
    log(i18n("Conflict: kept your version of %1 as %2", rel, nameOf(candidate)), QStringLiteral("conflict"), candidate);
    Q_EMIT conflictCreated(absPath(rel), nameOf(candidate));
    return candidate;
}

void SyncEngine::moveWaits(const QString &fromRel, const QString &toRel)
{
    // Not applied: the state keeps the old place, and the item is fetched again next cycle
    markError(fromRel);
    recordError(i18n("Moving %1 to %2 waits until the item there has moved away", fromRel, toRel));
}

bool SyncEngine::moveLocal(const QString &fromRel, const QString &toRel)
{
    m_moveWaits = false;
    // Parked earlier in this batch because something moved onto its place (see below)
    const QString from = absPath(m_parked.contains(fromRel) ? m_parked.take(fromRel) : fromRel);
    const QString to = absPath(toRel);
    if (!LocalScanner::stat(from)) {
        return false;
    }
    if (LocalScanner::stat(to)) {
        if (m_movingAway.contains(Placeholder::realPathOf(toRel)) && !m_parked.contains(toRel)) {
            // The item there moves away in this same batch (names swapped online): parked under a
            // visible name until its own move, never taken for a conflict
            const QString parked = toRel + QStringLiteral(".graphsync-moving");
            if (::rename(QFile::encodeName(to).constData(), QFile::encodeName(absPath(parked)).constData()) != 0) {
                markError(toRel);
                recordError(i18n("Cannot move %1 aside to make room for %2", toRel, fromRel));
                return false;
            }
            m_parked.insert(toRel, parked);
        } else if (const QString occupant = knownItemAt(Placeholder::realPathOf(toRel)); !occupant.isEmpty()) {
            // Another synced item is still there (the feed shows the moves of a swap one at a
            // time): this move waits for it to go, it is no conflict
            m_moveWaits = true;
            return false;
        } else if (makeConflictCopy(toRel).isEmpty()) {
            // Something unrelated sits at the destination and cannot be kept aside: never replace it
            return false;
        }
    }
    QDir().mkpath(QFileInfo(to).absolutePath());
    if (::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) != 0) {
        markError(fromRel);
        recordError(i18n("Cannot move %1 to %2", fromRel, toRel));
        return false;
    }
    ++m_stats.movedLocal;
    followPins(fromRel, toRel);
    log(i18n("Moved %1 to %2", fromRel, toRel), QStringLiteral("moved"), toRel);
    return true;
}

// ---------------------------------------------------------------------------------------
// Remote -> local

bool SyncEngine::remotePhase(QString *error)
{
    // Only full-listing requests made while this cycle runs count (see m_startOver)
    m_startOver = false;
    // Graph only lists "Add shortcut to My files" items when asked for them (undocumented; the
    // same header OneDrive's own clients send). A delta position from before this was asked for
    // never delivers the existing shortcuts, so such a root starts over once.
    const bool wantsShortcuts = m_root.kind == QLatin1String("personal");
    // hierarchicalsharing: only items with sharing of their own carry the "shared" facet (else
    // everything in a library would, through the site's members). Graph does not report sharing
    // changes in the feed for a user's request; they arrive with the item's next change, the
    // regular full check, or requestSharingCheck() after a share from this computer.
    const HeaderList deltaHeaders{{"Prefer", wantsShortcuts ? "Include-Feature=AddToOneDrive, hierarchicalsharing" : "hierarchicalsharing"}};
    if (wantsShortcuts && m_db->meta(QStringLiteral("shortcutsInDelta")) != QLatin1String("1")) {
        m_db->setMeta(QStringLiteral("deltaLink"), QString());
        m_db->setMeta(QStringLiteral("shortcutsInDelta"), QStringLiteral("1"));
    }
    // A delta position from before the sharing facet was asked for: start over once to learn it
    if (m_db->meta(QStringLiteral("sharingInDelta")) != QLatin1String("1")) {
        m_db->setMeta(QStringLiteral("deltaLink"), QString());
        m_db->setMeta(QStringLiteral("sharingInDelta"), QStringLiteral("1"));
    }

    // Regular full check next to the change feed, as a safety net
    if (m_settings.fullScanHours > 0 && !m_db->meta(QStringLiteral("deltaLink")).isEmpty()) {
        const QDateTime last = QDateTime::fromString(m_db->meta(QStringLiteral("lastFullScan")), Qt::ISODate);
        if (!last.isValid() || last.secsTo(QDateTime::currentDateTimeUtc()) > qint64(m_settings.fullScanHours) * 3600) {
            m_db->setMeta(QStringLiteral("deltaLink"), QString());
            log(i18n("Checking all files (regular full check)"));
        }
    }

    QString link = m_db->meta(QStringLiteral("deltaLink"));
    bool incremental = !link.isEmpty();
    const QString freshLink = QStringLiteral("/drives/%1/root/delta?$select=%2,shared").arg(m_root.driveId, DriveItem::selectFields());
    if (!incremental) {
        link = freshLink;
    }

    QList<DriveItem> changes;
    QHash<QString, qsizetype> indexById;
    QString nextDeltaLink;
    while (!link.isEmpty()) {
        if (m_cancel) {
            *error = i18n("Cancelled");
            return false;
        }
        const GraphResponse response = m_graph->get(link, deltaHeaders);
        if (response.status == 410) {
            // The token expired or the server wants a full resync
            log(i18n("Server asked for a full resync"));
            changes.clear();
            indexById.clear();
            incremental = false;
            link = freshLink;
            continue;
        }
        if (!response.ok()) {
            // No answer at all (offline, no DNS, sign-in server unreachable) is not a sync problem
            m_offline = response.status == 0 && !response.errorString.contains(QLatin1String("cancelled"))
                && !response.errorString.contains(QLatin1String("sign-in expired")) && !response.errorString.contains(QLatin1String("not signed in"));
            if (m_offline) {
                *error = i18n("No connection to Microsoft 365; syncing continues when the network is back.");
            } else if (response.errorString.contains(QLatin1String("sign-in expired")) || response.errorString.contains(QLatin1String("not signed in"))) {
                // The daemon also shows "Sign in again" with a button for this
                *error = i18n("Your sign-in expired. Sign in again to continue syncing.");
            } else {
                *error = i18n("Fetching changes failed: %1", response.describe());
            }
            return false;
        }
        const QJsonObject page = response.json();
        ++m_stats.deltaPages;
        const QJsonArray values = page.value(QLatin1String("value")).toArray();
        for (const QJsonValue &value : values) {
            DriveItem item = DriveItem::fromJson(value.toObject());
            item.sharingKnown = true;
            ++m_stats.deltaItems;
            // The delta shows the latest state per item; an item can appear more than once
            const auto existing = indexById.constFind(item.id);
            if (existing != indexById.constEnd()) {
                changes[*existing] = item;
            } else {
                indexById.insert(item.id, changes.size());
                changes.append(item);
            }
        }
        link = page.value(QLatin1String("@odata.nextLink")).toString();
        if (link.isEmpty()) {
            nextDeltaLink = page.value(QLatin1String("@odata.deltaLink")).toString();
        }
    }

    // Root of the drive (library root sync)
    if (m_root.remoteRootId.isEmpty()) {
        for (const DriveItem &item : std::as_const(changes)) {
            if (item.isRoot) {
                m_rootItemId = item.id;
                m_db->setMeta(QStringLiteral("rootItemId"), item.id);
            }
        }
        if (m_rootItemId.isEmpty()) {
            const GraphResponse root = m_graph->get(QStringLiteral("/drives/%1/root?$select=id").arg(m_root.driveId));
            if (!root.ok()) {
                *error = i18n("Cannot read the drive root: %1", root.describe());
                return false;
            }
            m_rootItemId = root.json().value(QLatin1String("id")).toString();
            m_db->setMeta(QStringLiteral("rootItemId"), m_rootItemId);
        }
    }

    // Items that failed in earlier cycles are fetched again one by one
    QHash<QString, int> attempts;
    QStringList unreachable;
    const QList<DriveItem> retryItems = fetchRetryItems(&attempts, &unreachable);
    for (const DriveItem &item : retryItems) {
        if (!indexById.contains(item.id)) {
            indexById.insert(item.id, changes.size());
            changes.append(item);
        }
    }

    m_failedIds.clear();
    for (const QString &id : std::as_const(unreachable)) {
        m_failedIds.insert(id);
    }
    m_db->begin();
    m_uncommitted = 0;
    m_seenRemote.clear();

    // Creations, renames, moves and content changes; a child is applied once its parent is known
    QList<DriveItem> pending;
    QList<DriveItem> shortcuts;
    QStringList deletes;
    bool ignoredShortcut = false;
    for (const DriveItem &item : std::as_const(changes)) {
        if (item.id == m_rootItemId) {
            if (item.deleted) {
                m_db->commit();
                *error = i18n("The synced folder was deleted online; stopping this sync root");
                return false;
            }
            continue;
        }
        if (item.isRoot) {
            continue;
        }
        if (item.deleted) {
            if (m_shortcuts.contains(item.id)) {
                forgetShortcut(item.id, i18n("it was removed online"));
            } else if (m_items.contains(item.id)) {
                deletes.append(item.id);
            }
            continue;
        }
        if (!DriveItem::isSafeName(item.name) || !DriveItem::isSafeId(item.id)) {
            qCWarning(GRAPHSYNC) << "Skipped an item with an unsafe name or id from the server";
            // Still there online: a full listing must not take what we have of it for deleted
            if (DriveItem::isSafeId(item.id)) {
                m_seenRemote.insert(item.id);
                if (const auto rel = relPath(item.id); rel && m_items.contains(item.id)) {
                    markError(*rel);
                    recordError(i18n("%1 got a name online that cannot be used here (too long or not allowed); it is kept here as it was", *rel));
                }
            }
            continue;
        }
        if (item.isRemoteShortcut) {
            // Shortcuts to folders and libraries; files added with "Add to My files" are not synced
            if (item.remoteIsFolder && !item.remoteDriveId.isEmpty() && !item.remoteId.isEmpty()) {
                shortcuts.append(item);
            } else {
                ignoredShortcut = true;
            }
            continue;
        }
        pending.append(item);
    }
    if (ignoredShortcut) {
        qCDebug(GRAPHSYNC) << "Shortcuts to single files are not synced";
    }

    // Known items that move in this batch, by their path before it: one moving onto the place of
    // another is not a conflict when that one moves away too (moveLocal parks it meanwhile)
    m_movingAway.clear();
    m_parked.clear();
    for (const DriveItem &item : std::as_const(pending)) {
        const auto oldRel = m_items.contains(item.id) ? relPath(item.id) : std::nullopt;
        const auto parentRel = item.parentId == m_rootItemId ? std::optional<QString>(QString()) : relPath(item.parentId);
        if (oldRel && (!parentRel || joinRel(*parentRel, item.name) != *oldRel)) {
            m_movingAway.insert(*oldRel);
        }
    }

    bool progress = true;
    while (!pending.isEmpty() && progress && !m_cancel) {
        progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
            const bool parentKnown = it->parentId == m_rootItemId || (m_items.contains(it->parentId) && m_items.value(it->parentId).isFolder);
            if (!parentKnown) {
                ++it;
                continue;
            }
            m_seenRemote.insert(it->id);
            const qsizetype errorsBefore = m_cycleErrors.size();
            applyRemoteItem(*it, incremental);
            if (m_cycleErrors.size() > errorsBefore) {
                m_failedIds.insert(it->id);
            }
            // Only the delta feed carries the sharing in the hierarchical sense (see above)
            // (retried items are fetched one by one and folder listings come from /children: neither says)
            if (const auto row = m_items.find(it->id); it->sharingKnown && row != m_items.end() && row->shared != it->shared) {
                row->shared = it->shared;
                saveRow(*row);
            }
            it = pending.erase(it);
            progress = true;
        }
    }
    // Parked items whose own move did not come: back to their place, or kept under a free name
    for (auto it = m_parked.constBegin(); it != m_parked.constEnd(); ++it) {
        QString target = it.key();
        if (LocalScanner::stat(absPath(target))) {
            const QString name = nameOf(target);
            target = joinRel(parentOf(target), name + QStringLiteral("-") + QSysInfo::machineHostName());
        }
        if (::rename(QFile::encodeName(absPath(it.value())).constData(), QFile::encodeName(absPath(target)).constData()) != 0) {
            recordError(i18n("%1 was moved aside and could not be put back; it is at %2", it.key(), it.value()));
        }
    }
    m_parked.clear();
    m_movingAway.clear();
    applyPins();
    autoFreeUp();
    runQueuedDownloads();
    if (m_lowSpace) {
        // Keep the delta position: the skipped downloads come back once there is room
        m_db->commit();
        *error = m_lowSpaceMessage;
        return false;
    }

    // Shortcuts once their parent folders exist; a full enumeration lists all that still exist
    QSet<QString> seenShortcuts;
    for (const DriveItem &item : std::as_const(shortcuts)) {
        seenShortcuts.insert(item.id);
        applyRemoteShortcut(item);
    }
    if (!incremental) {
        const QStringList known = m_shortcuts.keys();
        for (const QString &id : known) {
            if (!seenShortcuts.contains(id)) {
                forgetShortcut(id, i18n("it was removed online"));
            }
        }
    }
    saveShortcuts();

    // Stopped half-way (pause, shutdown): what is left was simply not applied yet, so nothing is
    // taken for deleted; the delta position stays, so the next cycle sees it all again
    if (m_cancel) {
        m_db->commit();
        *error = i18n("Cancelled");
        return false;
    }

    // Whatever is left lives outside the synced folder (folder roots see the whole drive)
    for (const DriveItem &item : std::as_const(pending)) {
        if (m_items.contains(item.id)) {
            // Moved out of the synced folder: from our point of view it was deleted
            deletes.append(item.id);
        }
    }

    // A full enumeration lists everything that exists; anything else we knew is gone
    if (!incremental) {
        for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
            if (!m_seenRemote.contains(it.key()) && !deletes.contains(it.key())) {
                deletes.append(it.key());
            }
        }
    }
    applyRemoteDeletes(deletes);
    m_db->commit();

    if (m_cancel) {
        *error = i18n("Cancelled");
        return false;
    }
    // The change token always moves on; items that failed are retried one by one in the next
    // cycles and given up after a few attempts, so one stuck file cannot hold up the rest
    QJsonObject retry;
    for (const QString &id : std::as_const(m_failedIds)) {
        const int count = attempts.value(id) + 1;
        if (count < maxItemAttempts) {
            retry.insert(id, count);
        } else {
            const QString message = i18n("Gave up on an item after %1 attempts; see the activity list for the error", count);
            {
                QMutexLocker locker(&m_statusMutex);
                QStringList persistent = m_status.value(QStringLiteral("persistentErrors")).toStringList();
                persistent.append(message);
                m_status.insert(QStringLiteral("persistentErrors"), persistent);
            }
            Q_EMIT persistentError(message);
        }
    }
    m_db->setMeta(QStringLiteral("retry"), QString::fromUtf8(QJsonDocument(retry).toJson(QJsonDocument::Compact)));
    if (std::exchange(m_startOver, false)) {
        // Asked for a full listing while this cycle ran: the next one starts over
        m_db->setMeta(QStringLiteral("deltaLink"), QString());
    } else if (!nextDeltaLink.isEmpty()) {
        m_db->setMeta(QStringLiteral("deltaLink"), nextDeltaLink);
        if (!incremental) {
            m_db->setMeta(QStringLiteral("lastFullScan"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        }
    }
    return true;
}

void SyncEngine::applyRemoteItem(const DriveItem &item, bool incremental)
{
    const auto parentRel = relPath(item.parentId);
    if (!parentRel) {
        return;
    }
    const QString newRel = joinRel(*parentRel, item.name);
    if (isLeftAlone(newRel)) {
        // Ignored names and the user's exclusions: not synced either way, existing copies stay as they are
        return;
    }
    if (isExcluded(newRel)) {
        // Not synced here (Choose folders); a known item moved into an excluded folder leaves
        if (m_items.contains(item.id)) {
            deleteLocalSubtree(item.id, true);
        }
        return;
    }
    auto existing = m_items.find(item.id);
    if (existing != m_items.end() && existing->isFolder != item.isFolder) {
        // A file became a folder or the other way round: forget the old row
        dropRow(item.id);
        m_items.erase(existing);
        existing = m_items.end();
    }

    if (item.isFolder) {
        ItemRow row = existing != m_items.end() ? *existing : ItemRow{};
        const bool isNew = existing == m_items.end();
        if (!isNew) {
            const auto oldRel = relPath(item.id);
            if (oldRel && *oldRel != newRel && !moveLocal(*oldRel, newRel) && m_moveWaits) {
                moveWaits(*oldRel, newRel);
                return;
            }
        } else {
            const auto local = LocalScanner::stat(absPath(newRel));
            if (local && !local->isFolder && makeConflictCopy(newRel).isEmpty()) {
                return;
            }
            if (!QDir().mkpath(absPath(newRel))) {
                markError(newRel);
                recordError(i18n("Cannot create folder %1", newRel));
                return;
            }
        }
        row.id = item.id;
        row.parentId = item.parentId;
        row.name = item.name;
        row.isFolder = true;
        row.remoteETag = item.eTag;
        row.remoteCTag = item.cTag;
        storeLocalBaseline(row, newRel, {});
        m_items.insert(row.id, row);
        saveRow(row);
        if (isNew && incremental && !m_root.remoteRootId.isEmpty()) {
            // Moved into the synced folder from elsewhere: its unchanged children are not in the delta
            enumerateNewFolder(item.id);
        }
        return;
    }

    if (existing != m_items.end() && existing->dehydrated) {
        // Online-only: the placeholder follows renames and moves and shows the new size
        ItemRow row = *existing;
        const auto oldRel = relPath(item.id);
        if (oldRel && *oldRel != newRel && !moveLocal(*oldRel + Placeholder::suffix, newRel + Placeholder::suffix) && m_moveWaits) {
            moveWaits(*oldRel, newRel);
            return;
        }
        const bool contentChanged = remoteContentChanged(row, item);
        row.parentId = item.parentId;
        row.name = item.name;
        row.remoteETag = item.eTag;
        row.remoteCTag = item.cTag;
        row.remoteHash = item.quickXorHash;
        row.remoteSize = item.size;
        if (contentChanged || !QFileInfo::exists(absPath(newRel) + Placeholder::suffix)) {
            QString error;
            if (!Placeholder::write(absPath(newRel), item.size, item.modified, &error)) {
                markError(newRel);
                recordError(i18n("Cannot write the placeholder for %1: %2", newRel, error));
            }
        }
        if (const auto placeholder = LocalScanner::stat(absPath(newRel) + Placeholder::suffix)) {
            row.localInode = placeholder->inode;
        }
        m_items.insert(row.id, row);
        saveRow(row);
        bool explicitPin = false;
        if (effectivePin(newRel, &explicitPin) == StateDb::Pin::AlwaysKeep) {
            queueDownload(item, newRel);
        }
        return;
    }

    if (existing != m_items.end()) {
        ItemRow row = *existing;
        const auto oldRel = relPath(item.id);
        if (oldRel && *oldRel != newRel && !moveLocal(*oldRel, newRel) && m_moveWaits) {
            moveWaits(*oldRel, newRel);
            return;
        }
        row.parentId = item.parentId;
        row.name = item.name;
        m_items.insert(row.id, row);

        const bool contentChanged = remoteContentChanged(row, item);
        const auto local = LocalScanner::stat(absPath(newRel));
        if (!contentChanged) {
            row.remoteETag = item.eTag;
            row.remoteCTag = item.cTag;
            if (local && !local->isFolder) {
                // Keep the inode current so a later local rename is still recognised
                row.localInode = local->inode;
            }
            m_items.insert(row.id, row);
            saveRow(row);
            return;
        }
        // Changed here too: kept aside first; when that is impossible, nothing is downloaded over
        // it (the item is tried again in the next cycle)
        if (local && !local->isFolder && localChangedSinceBaseline(row, *local, newRel) && makeConflictCopy(newRel).isEmpty()) {
            return;
        }
        queueDownload(item, newRel);
        return;
    }

    // New to us
    const auto local = LocalScanner::stat(absPath(newRel));
    bool explicitPin = false;
    if (m_pins.value(newRel) == StateDb::Pin::Available) {
        // Left by an earlier file at this path that was opened; a new one gets its folder's choice
        m_db->removePins({newRel});
        m_pins = m_db->loadPins();
    }
    if (!local && effectivePin(newRel, &explicitPin) == StateDb::Pin::OnlineOnly) {
        // Files On-Demand: only a placeholder until it is opened or pinned
        createPlaceholder(item, newRel);
        return;
    }
    if (local && local->isFolder) {
        if (makeConflictCopy(newRel).isEmpty()) {
            return;
        }
    } else if (local) {
        const QString hash = QuickXorHash::hashFile(absPath(newRel));
        if (!hash.isEmpty() && hash == item.quickXorHash) {
            // Same content already here (first sync into a pre-filled folder): adopt it
            ItemRow row;
            row.id = item.id;
            row.parentId = item.parentId;
            row.name = item.name;
            row.remoteETag = item.eTag;
            row.remoteCTag = item.cTag;
            row.remoteHash = item.quickXorHash;
            row.remoteSize = item.size;
            storeLocalBaseline(row, newRel, hash);
            m_items.insert(row.id, row);
            saveRow(row);
            return;
        }
        if (makeConflictCopy(newRel).isEmpty()) {
            return;
        }
    }
    queueDownload(item, newRel);
}

void SyncEngine::enumerateNewFolder(const QString &folderId)
{
    QString link = itemPath(folderId) + QStringLiteral("/children?$select=") + DriveItem::selectFields();
    while (!link.isEmpty() && !m_cancel) {
        const GraphResponse response = m_graph->get(link);
        if (!response.ok()) {
            recordError(i18n("Listing a folder that was moved in failed: %1", response.describe()));
            return;
        }
        const QJsonObject page = response.json();
        const QJsonArray values = page.value(QLatin1String("value")).toArray();
        for (const QJsonValue &value : values) {
            const DriveItem child = DriveItem::fromJson(value.toObject());
            if (!DriveItem::isSafeName(child.name) || !DriveItem::isSafeId(child.id)) {
                qCWarning(GRAPHSYNC) << "Skipped an item with an unsafe name or id from the server";
                continue;
            }
            m_seenRemote.insert(child.id);
            applyRemoteItem(child, true);
        }
        link = page.value(QLatin1String("@odata.nextLink")).toString();
    }
}

void SyncEngine::queueDownload(const DriveItem &item, const QString &rel)
{
    // Once per item: a new folder is listed twice in a folder location (its own listing and the
    // delta), and two downloads of one item would share its temporary file. The latest wins.
    if (const auto at = m_downloadIndex.constFind(item.id); at != m_downloadIndex.constEnd() && *at < m_downloadQueue.size()
        && m_downloadQueue.at(*at).item.id == item.id) {
        m_downloadQueue[*at] = {item, rel};
        return;
    }
    m_downloadIndex.insert(item.id, m_downloadQueue.size());
    m_downloadQueue.append({item, rel});
}

QString SyncEngine::tempPathFor(const DriveItem &item, const QString &rel) const
{
    const QString dir = QFileInfo(absPath(rel)).absolutePath();
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + IgnoreRules::tempPrefix + item.id;
}

void SyncEngine::runQueuedDownloads()
{
    const QList<QueuedDownload> queue = std::exchange(m_downloadQueue, {});
    m_downloadIndex.clear();
    QStringList busy;
    for (const QueuedDownload &entry : queue) {
        busy.append(entry.rel);
    }
    setBusy(busy);
    const qint64 reserve = qint64(m_settings.minimumFreeSpaceMB) * 1024 * 1024;
    for (qsizetype start = 0; start < queue.size() && !m_cancel && !m_lowSpace; start += transferBatch) {
        // Someone is waiting for a file they opened: that comes first
        m_atSafePoint = true;
        processRequests();
        m_atSafePoint = false;
        const qsizetype end = std::min(start + transferBatch, queue.size());
        std::vector<std::unique_ptr<QFile>> files;
        QList<ParallelJob> jobs;
        // Never fill the disk: what does not fit waits for the next cycle
        QStorageInfo storage(m_root.localPath);
        qint64 room = storage.isValid() ? storage.bytesAvailable() - reserve : std::numeric_limits<qint64>::max();
        qint64 batchBytes = 0;
        const bool limited = RateLimiter::downloadLimit() > 0;
        QList<QueuedDownload> paced;
        for (qsizetype i = start; i < end; ++i) {
            const QueuedDownload &entry = queue.at(i);
            if (entry.item.size > room) {
                m_lowSpace = true;
                m_lowSpaceMessage = i18n("Not enough free disk space for %1 (%2). %3 is kept free; syncing continues when there is room.",
                                         entry.rel,
                                         QLocale().formattedDataSize(entry.item.size),
                                         QLocale().formattedDataSize(reserve));
                break;
            }
            room -= entry.item.size;
            batchBytes += entry.item.size;
            if (limited) {
                paced.append(entry);
                continue;
            }
            auto file = std::make_unique<QFile>(tempPathFor(entry.item, entry.rel));
            if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                recordError(i18n("Cannot write %1", file->fileName()));
                m_failedIds.insert(entry.item.id);
                continue;
            }
            ParallelJob job;
            job.pathOrUrl = itemPath(entry.item.id) + QStringLiteral("/content");
            job.sink = file.get();
            QFile *raw = file.get();
            job.done = [this, entry, raw](const GraphResponse &response) {
                raw->close();
                const qsizetype errorsBefore = m_cycleErrors.size();
                finishDownload(entry.item, entry.rel, raw->fileName(), response);
                if (m_cycleErrors.size() > errorsBefore) {
                    m_failedIds.insert(entry.item.id);
                }
            };
            jobs.append(job);
            files.push_back(std::move(file));
        }
        if (limited) {
            // Download limit: one file at a time, in paced pieces
            beginTransfers(static_cast<int>(paced.size()), batchBytes);
            for (const QueuedDownload &entry : std::as_const(paced)) {
                if (m_cancel) {
                    break;
                }
                const qsizetype errorsBefore = m_cycleErrors.size();
                downloadInPieces(entry.item, entry.rel);
                if (m_cycleErrors.size() > errorsBefore) {
                    m_failedIds.insert(entry.item.id);
                }
            }
            continue;
        }
        beginTransfers(static_cast<int>(jobs.size()), batchBytes);
        m_graph->runParallel(jobs, transferConcurrency);
    }
}

bool SyncEngine::downloadInPieces(const DriveItem &item, const QString &rel)
{
    // Pieces of about one second of the limit: each is fetched at full speed, and the pacing in
    // between keeps the average at the limit (Qt buffers a reply regardless of how fast it is read)
    const qint64 piece = std::clamp<qint64>(RateLimiter::downloadLimit(), 64 * 1024, 8 * 1024 * 1024);
    const QString tmp = tempPathFor(item, rel);
    QFile file(tmp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        transferFinished();
        recordError(i18n("Cannot write %1", tmp));
        return false;
    }
    const QString path = itemPath(item.id) + QStringLiteral("/content");
    GraphResponse response;
    qint64 offset = 0;
    while (!m_cancel) {
        const bool whole = item.size <= piece;
        const qint64 length = whole ? item.size : std::min(piece, item.size - offset);
        RateLimiter::waitForDownload(std::max<qint64>(length, 1), &m_cancel);
        HeaderList headers;
        if (!whole) {
            headers.emplaceBack(QByteArrayLiteral("Range"), "bytes=" + QByteArray::number(offset) + '-' + QByteArray::number(offset + length - 1));
        }
        response = m_graph->get(path, headers);
        if (!response.ok()) {
            break;
        }
        if (response.status == 200 && offset > 0) {
            // The whole file instead of the piece asked for (no range support on the way): it
            // replaces what came before, never appended to it
            file.resize(0);
            file.seek(0);
            m_transfer.bytesDone -= offset;
            offset = 0;
        }
        file.write(response.body);
        m_transfer.bytesDone += response.body.size();
        publishTransfer(false);
        offset += response.body.size();
        // 200 = the server sent the whole file (no range support); done either way
        if (whole || response.status == 200 || response.body.isEmpty() || offset >= item.size) {
            break;
        }
    }
    file.close();
    if (m_cancel && response.ok()) {
        response = {0, {}, QStringLiteral("cancelled")};
    } else if (response.ok() && offset != item.size) {
        // Short or too long: never taken for the file, also when there is no hash to check it
        response = {0, {}, QStringLiteral("the download stopped at %1 of %2 bytes").arg(offset).arg(item.size)};
    }
    return finishDownload(item, rel, tmp, response);
}

bool SyncEngine::download(const DriveItem &item, const QString &rel)
{
    beginTransfers(1, item.size);
    if (RateLimiter::downloadLimit() > 0) {
        return downloadInPieces(item, rel);
    }
    const QString tmp = tempPathFor(item, rel);
    QFile file(tmp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        recordError(i18n("Cannot write %1", tmp));
        return false;
    }
    const GraphResponse response = m_graph->download(itemPath(item.id) + QStringLiteral("/content"), &file);
    file.close();
    return finishDownload(item, rel, tmp, response);
}

bool SyncEngine::finishDownload(const DriveItem &item, const QString &rel, const QString &tmp, const GraphResponse &response)
{
    const auto finished = qScopeGuard([this] {
        transferFinished();
    });
    const QString abs = absPath(rel);
    if (!response.ok()) {
        QFile::remove(tmp);
        markError(rel);
        // Also the text of the notification when opening an online-only file fails
        recordError(response.status == 0 && !response.errorString.contains(QLatin1String("cancelled"))
                        ? i18n("Download of %1 failed: no connection to Microsoft 365", rel)
                        : i18n("Download of %1 failed: %2", rel, response.describe()));
        return false;
    }
    const QString hash = QuickXorHash::hashFile(tmp);
    if (!item.quickXorHash.isEmpty() && hash != item.quickXorHash) {
        // Usually the file changed online between listing and downloading; the next cycle retries
        QFile::remove(tmp);
        markError(rel);
        recordError(i18n("Download of %1 did not match the server hash; retrying next cycle", rel));
        return false;
    }
    LocalScanner::setModified(tmp, item.modified);
    // What is at the path now must be what this download replaces: nothing (a new or online-only
    // file) or the file as last synced. Anything else is local work since the download was queued
    // (a new file saved where the placeholder was, an edit meanwhile): it is kept aside first, and
    // when that is impossible the download is dropped (tried again next cycle).
    if (const auto here = LocalScanner::stat(abs); here && !here->isFolder) {
        const auto known = m_items.constFind(item.id);
        const bool replaceable = known != m_items.constEnd() && !known->dehydrated && !localChangedSinceBaseline(*known, *here, rel);
        if (!replaceable && makeConflictCopy(rel).isEmpty()) {
            QFile::remove(tmp);
            return false;
        }
    }
    if (::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(abs).constData()) != 0) {
        QFile::remove(tmp);
        markError(rel);
        recordError(i18n("Cannot move the downloaded file into place: %1", rel));
        return false;
    }

    ItemRow row = m_items.value(item.id);
    if (row.dehydrated) {
        // The real file is in place: the placeholder goes
        QFile::remove(abs + Placeholder::suffix);
        row.dehydrated = false;
    }
    row.id = item.id;
    row.parentId = item.parentId;
    row.name = item.name;
    row.isFolder = false;
    row.remoteETag = item.eTag;
    row.remoteCTag = item.cTag;
    row.remoteHash = item.quickXorHash.isEmpty() ? hash : item.quickXorHash;
    row.remoteSize = item.size;
    storeLocalBaseline(row, rel, hash);
    m_items.insert(row.id, row);
    saveRow(row);
    ++m_stats.downloaded;
    log(i18n("Downloaded %1", rel), QStringLiteral("downloaded"), rel, item.modifiedById != m_selfId ? item.modifiedByName : QString());
    return true;
}

void SyncEngine::applyRemoteDeletes(const QStringList &ids)
{
    // Shallowest first: a folder that can go to the Trash as a whole takes its contents along,
    // and one that cannot handles its children itself (deleteLocalSubtree)
    QList<QPair<int, QString>> ordered;
    for (const QString &id : ids) {
        if (const auto rel = relPath(id)) {
            ordered.append({depthOf(*rel), id});
        } else {
            m_items.remove(id);
            dropRow(id);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });
    DeleteContext context = deleteContext();
    // Deleted and made again under the same name in the same batch: the new item has adopted
    // what is here, so the old one only leaves the state, and nothing here is touched
    const QSet<QString> gone(ids.cbegin(), ids.cend());
    QHash<QString, QString> livePaths;
    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        if (!gone.contains(it.key())) {
            if (const auto rel = relPath(it.key())) {
                livePaths.insert(*rel, it.key());
            }
        }
    }
    for (const auto &[depth, id] : std::as_const(ordered)) {
        Q_UNUSED(depth)
        if (!m_items.contains(id)) {
            continue;
        }
        if (const auto rel = relPath(id); rel && livePaths.contains(*rel)) {
            dropSubtreeRows(id, context);
            continue;
        }
        deleteLocalSubtree(id, false, context, false);
    }
}

void SyncEngine::dropSubtreeRows(const QString &id, const DeleteContext &context)
{
    const QList<QString> children = context.children.values(id);
    for (const QString &child : children) {
        if (m_items.contains(child)) {
            dropSubtreeRows(child, context);
        }
    }
    m_items.remove(id);
    dropRow(id);
}

SyncEngine::DeleteContext SyncEngine::deleteContext() const
{
    DeleteContext context;
    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        context.children.insert(it->parentId, it.key());
    }
    return context;
}

void SyncEngine::deleteLocalSubtree(const QString &id, bool becauseExcluded)
{
    DeleteContext context = deleteContext();
    deleteLocalSubtree(id, becauseExcluded, context, false);
}

void SyncEngine::deleteLocalSubtree(const QString &id, bool becauseExcluded, DeleteContext &context, bool checked)
{
    const auto rel = relPath(id);
    const ItemRow row = m_items.value(id);
    if (!rel) {
        m_items.remove(id);
        dropRow(id);
        return;
    }

    if (row.isFolder) {
        // One check per folder handed to us; a parent's check already covers its subfolders
        if (!checked) {
            checkSubtree(id, *rel, context);
        }
        const QString prefix = *rel + QLatin1Char('/');
        const bool clean = std::none_of(context.dirty.cbegin(), context.dirty.cend(), [&](const QString &path) {
            return path == *rel || path.startsWith(prefix);
        });
        // Everything inside synced and unchanged here: one Trash entry for the whole folder,
        // instead of one per file (10,000 files made 10,011 entries)
        if (clean && trashWholeFolder(id, *rel, becauseExcluded, context)) {
            return;
        }
        const QList<QString> children = context.children.values(id);
        for (const QString &child : children) {
            if (m_items.contains(child)) {
                deleteLocalSubtree(child, becauseExcluded, context, true);
            }
        }
        const QString abs = absPath(*rel);
        QDir dir(abs);
        if (dir.exists()) {
            const QStringList left = dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
            const bool onlyIgnored = std::all_of(left.begin(), left.end(), [](const QString &name) {
                return IgnoreRules::isIgnored(name);
            });
            if (onlyIgnored) {
                if (QFile::moveToTrash(abs)) {
                    ++m_stats.deletedLocal;
                    log(becauseExcluded ? i18n("Removed the local copy of folder %1 (no longer synced, moved to Trash)", *rel)
                                        : i18n("Deleted folder %1 (moved to Trash): it was deleted online", *rel),
                        QStringLiteral("deleted"),
                        *rel);
                }
            } else {
                log(becauseExcluded ? i18n("Kept folder %1: no longer synced, but it holds new or changed files that were not uploaded", *rel)
                                    : i18n("Kept folder %1: deleted online, but it holds new local files", *rel));
            }
        }
    } else if (row.dehydrated) {
        // Online-only: only the placeholder is here
        QFile::remove(absPath(*rel) + Placeholder::suffix);
    } else {
        const auto local = LocalScanner::stat(absPath(*rel));
        if (local && !local->isFolder) {
            if (!localChangedSinceBaseline(row, *local, *rel)) {
                if (QFile::moveToTrash(absPath(*rel))) {
                    ++m_stats.deletedLocal;
                    log(becauseExcluded ? i18n("Removed the local copy of %1 (no longer synced, moved to Trash)", *rel)
                                        : i18n("Deleted %1 (moved to Trash): it was deleted online", *rel),
                        QStringLiteral("deleted"),
                        *rel);
                } else {
                    markError(*rel);
                    recordError(i18n("Cannot move %1 to the Trash", *rel));
                    return;
                }
            } else {
                // Local edits win: the file becomes new and is uploaded again
                log(becauseExcluded ? i18n("Kept %1: no longer synced, but changed here and not uploaded", *rel)
                                    : i18n("Kept %1: deleted online, but changed here", *rel));
            }
        }
    }
    m_items.remove(id);
    dropRow(id);
}

void SyncEngine::checkSubtree(const QString &id, const QString &rel, DeleteContext &context) const
{
    context.scanned.clear();
    context.dirty.clear();

    QHash<QString, QString> idByPath;
    QStringList pending{id};
    while (!pending.isEmpty()) {
        const QList<QString> children = context.children.values(pending.takeLast());
        for (const QString &child : children) {
            if (!m_items.contains(child)) {
                continue;
            }
            pending.append(child);
            if (const auto childRel = relPath(child)) {
                idByPath.insert(*childRel, child);
            }
        }
    }

    // Every local entry must be a synced item that did not change here (or a placeholder of an
    // online-only one). Ignored names (temp files) are left out by the scanner and go along
    const QString abs = absPath(rel);
    const QHash<QString, LocalEntry> local = LocalScanner::scan(abs);
    for (auto it = local.constBegin(); it != local.constEnd(); ++it) {
        const QString entryRel = rel + QLatin1Char('/') + it.key();
        context.scanned.insert(entryRel, *it);
        if (!it->isFolder && entryRel.endsWith(Placeholder::suffix)) {
            const QString childId = idByPath.value(entryRel.chopped(Placeholder::suffix.size()));
            if (!childId.isEmpty() && m_items.value(childId).dehydrated) {
                continue;
            }
        }
        const QString childId = idByPath.value(entryRel);
        const ItemRow row = m_items.value(childId);
        if (childId.isEmpty() || row.isFolder != it->isFolder
            || (!row.isFolder && (row.dehydrated || localChangedSinceBaseline(row, *it, entryRel)))) {
            context.dirty.append(entryRel);
        }
    }
    // The scanner leaves out symlinks, sockets and fifos; those were made here and never go along
    QDirIterator walk(abs, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    const QDir base(abs);
    while (walk.hasNext()) {
        const QFileInfo info(walk.next());
        if (info.isSymLink() || (!info.isFile() && !info.isDir())) {
            context.dirty.append(rel + QLatin1Char('/') + base.relativeFilePath(info.filePath()));
        }
    }
}

bool SyncEngine::trashWholeFolder(const QString &id, const QString &rel, bool becauseExcluded, const DeleteContext &context)
{
    const QString abs = absPath(rel);
    const QFileInfo info(abs);
    if (info.exists() || info.isSymLink()) {
        if (!info.isDir() || info.isSymLink()) {
            return false;
        }
        // Checked again right before moving (the check may have taken a while hashing): anything
        // created or changed since keeps the item-by-item path, which keeps it
        const QHash<QString, LocalEntry> now = LocalScanner::scan(abs);
        for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
            const auto was = context.scanned.constFind(rel + QLatin1Char('/') + it.key());
            if (was == context.scanned.constEnd() || was->size != it->size || was->mtimeNs != it->mtimeNs) {
                return false;
            }
        }
        if (!QFile::moveToTrash(abs)) {
            return false;
        }
        ++m_stats.deletedLocal;
        log(becauseExcluded ? i18n("Removed the local copy of folder %1 (no longer synced, moved to Trash)", rel)
                            : i18n("Deleted folder %1 (moved to Trash): it was deleted online", rel),
            QStringLiteral("deleted"),
            rel);
    }
    QStringList pending{id};
    while (!pending.isEmpty()) {
        const QString current = pending.takeLast();
        const QList<QString> children = context.children.values(current);
        for (const QString &child : children) {
            if (m_items.contains(child)) {
                pending.append(child);
            }
        }
        m_items.remove(current);
        dropRow(current);
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Local -> remote

void SyncEngine::rebuildPathIndex()
{
    m_pathToId.clear();
    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        if (const auto rel = relPath(it.key())) {
            m_pathToId.insert(*rel, it.key());
        }
    }
}

bool SyncEngine::localPhase(QString *error)
{
    // Shortcut folders are listed (a rename is recognised by inode) but not descended into
    const QHash<QString, LocalEntry> fullScan = LocalScanner::scan(m_root.localPath, shortcutPaths());
    QHash<QString, LocalEntry> scan = fullScan;
    m_noSyncCache.clear();
    const auto leftAlone = [this](const auto &it) {
        return isExcluded(it.key()) || isUnderShortcut(it.key()) || isLeftAlone(it.key());
    };
    scan.removeIf(leftAlone);
    rebuildPathIndex();

    // Files On-Demand: <rel>.cloud stands for an online-only file and is never uploaded. A
    // placeholder renamed or moved here (same inode) renames or moves the file online.
    // The placeholders are looked up in the full scan: *.cloud names are left alone, so the
    // filtered scan no longer has them, and without its placeholder an online-only file would
    // look deleted here and be deleted online.
    QSet<QString> placeholderPresent;
    QHash<quint64, QString> dehydratedByInode;
    // Items moved or renamed here whose move online did not (yet) work: never deleted online for
    // missing at their old path; for folders, everything that was inside them too
    QSet<QString> keepIds;
    QStringList keepFolders;
    // Placeholders moved into a folder that is only created online further down
    QList<QPair<QString, QString>> laterPlaceholderMoves;
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (!it->dehydrated) {
            continue;
        }
        const auto rel = relPath(it.key());
        if (!rel) {
            continue;
        }
        if (fullScan.contains(*rel + Placeholder::suffix)) {
            placeholderPresent.insert(*rel);
        } else if (it->localInode) {
            dehydratedByInode.insert(it->localInode, it.key());
        }
    }
    for (auto it = fullScan.constBegin(); it != fullScan.constEnd(); ++it) {
        const QString &key = it.key();
        if (!Placeholder::hasSuffix(key) || placeholderPresent.contains(Placeholder::realPathOf(key)) || isExcluded(key) || isUnderShortcut(key)
            || !Placeholder::isPlaceholder(absPath(key))) {
            continue;
        }
        const QString realRel = Placeholder::realPathOf(key);
        const QString movedId = dehydratedByInode.value(it->inode);
        if (movedId.isEmpty()) {
            continue;
        }
        keepIds.insert(movedId);
        if (moveRemote(movedId, realRel)) {
            dehydratedByInode.remove(it->inode);
            placeholderPresent.insert(realRel);
            rebuildPathIndex();
        } else {
            laterPlaceholderMoves.append({movedId, realRel});
        }
        // A copy of a placeholder is not a file; *.cloud names never reach the upload below
    }

    QHash<quint64, QString> inodeToId;
    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        if (it->localInode) {
            inodeToId.insert(it->localInode, it.key());
        }
    }

    // Big-delete guard: an unmounted disk or an emptied folder must not wipe the library. What is
    // still here under another path (same inode: moved or renamed) is not missing.
    QSet<quint64> inodesHere;
    for (auto it = fullScan.constBegin(); it != fullScan.constEnd(); ++it) {
        inodesHere.insert(it->inode);
    }
    QSet<QString> missingIds;
    for (auto it = m_pathToId.constBegin(); it != m_pathToId.constEnd(); ++it) {
        if (!scan.contains(it.key()) && !placeholderPresent.contains(it.key()) && !isLeftAlone(it.key()) && !isExcluded(it.key())
            && !isUnderShortcut(it.key()) && !keepIds.contains(it.value()) && !inodesHere.contains(m_items.value(it.value()).localInode)) {
            missingIds.insert(it.value());
        }
    }
    const int missing = static_cast<int>(missingIds.size());
    const int known = static_cast<int>(m_pathToId.size());
    const int decision = std::exchange(m_bigDeleteDecision, 0);
    // Also everything at once in a small location (an unmounted disk whose mount point is left)
    const bool everything = known > 1 && missing == known;
    // "Delete online too" answers the question about the items missing then: when more went
    // missing since (a disk unmounted after the question), it is asked again
    const bool approved = decision == 1 && QSet<QString>(missingIds).subtract(m_bigDeleteAskedFor).isEmpty();
    if (((missing > bigDeleteMinimum && missing > known * bigDeleteShare) || everything) && !approved) {
        if (decision == 2) {
            // Restore: forget the missing items so a full resync downloads them again
            m_db->begin();
            for (auto it = m_pathToId.constBegin(); it != m_pathToId.constEnd(); ++it) {
                if (!scan.contains(it.key())) {
                    m_items.remove(it.value());
                    dropRow(it.value());
                }
            }
            m_db->commit();
            m_forceResync = true;
            log(i18n("Restoring %1 items that disappeared locally", missing));
            return true;
        }
        {
            QMutexLocker locker(&m_statusMutex);
            m_status.insert(QStringLiteral("pendingBigDelete"), QVariantMap{{QStringLiteral("missing"), missing}, {QStringLiteral("known"), known}});
            m_bigDeleteAskedFor = missingIds;
        }
        Q_EMIT bigDeletePending(missing, known);
        *error = i18n("%1 of %2 synced items disappeared locally. Nothing was deleted online; choose to delete them online or to restore them.",
                      missing,
                      known);
        return false;
    }

    // Only past the guard: an emptied folder must not remove the shortcuts online either. A
    // shortcut folder renamed here has a new path now, so filter again.
    handleLocalShortcutChanges(fullScan);
    scan.removeIf(leftAlone);

    m_db->begin();
    m_uncommitted = 0;
    QSet<QString> movedIds;
    // New paths of folders whose move online failed: their contents wait
    QStringList stalledFolders;

    // Folders first, shallowest first, so every parent exists before its children
    QStringList folders;
    QStringList files;
    for (auto it = scan.constBegin(); it != scan.constEnd(); ++it) {
        (it->isFolder ? folders : files).append(it.key());
    }
    std::sort(folders.begin(), folders.end(), [](const QString &a, const QString &b) {
        return depthOf(a) != depthOf(b) ? depthOf(a) < depthOf(b) : a < b;
    });
    files.sort();

    for (const QString &rel : std::as_const(folders)) {
        if (m_cancel) {
            break;
        }
        const LocalEntry entry = scan.value(rel);
        const QString id = m_pathToId.value(rel);
        if (!id.isEmpty()) {
            ItemRow &row = m_items[id];
            if (row.localInode != entry.inode) {
                row.localInode = entry.inode;
                saveRow(row);
            }
            continue;
        }
        // Below a folder whose move failed: wait for it (no uploads into a folder not online)
        if (std::any_of(stalledFolders.cbegin(), stalledFolders.cend(), [&](const QString &stalled) {
                return rel.startsWith(stalled + QLatin1Char('/'));
            })) {
            continue;
        }
        const QString movedId = inodeToId.value(entry.inode);
        if (!movedId.isEmpty() && m_items.value(movedId).isFolder && !movedIds.contains(movedId)) {
            const auto oldRel = relPath(movedId);
            if (oldRel && !scan.contains(*oldRel)) {
                if (!moveRemoteOrKeep(movedId, *oldRel, rel)) {
                    // Stays where it is online with everything in it, until the move works
                    keepIds.insert(movedId);
                    keepFolders.append(*oldRel);
                    stalledFolders.append(rel);
                    continue;
                }
                movedIds.insert(movedId);
                rebuildPathIndex();
                continue;
            }
        }
        if (const QString reason = IgnoreRules::invalidRemoteNameReason(nameOf(rel)); !reason.isEmpty()) {
            markError(rel);
            recordError(i18n("Not uploading folder %1: the name %2", rel, reason));
            continue;
        }
        createRemoteFolder(rel, entry.inode);
    }

    // Placeholders moved into folders that exist online now
    for (const auto &[id, realRel] : std::as_const(laterPlaceholderMoves)) {
        if (moveRemoteOrKeep(id, relPath(id).value_or(QString()), realRel)) {
            placeholderPresent.insert(realRel);
            rebuildPathIndex();
        }
    }

    for (const QString &rel : std::as_const(files)) {
        if (m_cancel) {
            break;
        }
        if (std::any_of(stalledFolders.cbegin(), stalledFolders.cend(), [&](const QString &stalled) {
                return rel.startsWith(stalled + QLatin1Char('/'));
            })) {
            continue;
        }
        const LocalEntry entry = scan.value(rel);
        QString id = m_pathToId.value(rel);
        if (id.isEmpty()) {
            const QString movedId = inodeToId.value(entry.inode);
            // A rename keeps the modification time; a new file that got a deleted file's inode
            // number does not (then it is a new file, and the old one was deleted)
            const ItemRow moved = m_items.value(movedId);
            if (!movedId.isEmpty() && !moved.isFolder && !movedIds.contains(movedId) && (moved.dehydrated || moved.localMtimeNs == entry.mtimeNs)) {
                const auto oldRel = relPath(movedId);
                if (oldRel && !scan.contains(*oldRel)) {
                    if (moveRemoteOrKeep(movedId, *oldRel, rel)) {
                        movedIds.insert(movedId);
                        id = movedId;
                        m_pathToId.remove(*oldRel);
                        m_pathToId.insert(rel, id);
                    } else {
                        keepIds.insert(movedId);
                        continue;
                    }
                }
            }
        }
        // Only a new, changed or online-only path can be a placeholder that lost its suffix;
        // reading every small file each cycle would also count as opening it (automatic free up)
        const auto known = id.isEmpty() ? m_items.constEnd() : m_items.constFind(id);
        const bool syncedAndUnchanged = known != m_items.constEnd() && !known->dehydrated && entry.size == known->localSize && entry.mtimeNs == known->localMtimeNs;
        if (!syncedAndUnchanged && entry.size <= 4096 && Placeholder::isPlaceholder(absPath(rel))) {
            // A placeholder that lost its .cloud suffix: never upload it; put the suffix back
            // when it stands for an online-only file
            const QString owner = !id.isEmpty() ? id : dehydratedByInode.value(entry.inode);
            if (!owner.isEmpty() && m_items.value(owner).dehydrated) {
                const auto ownerRel = relPath(owner);
                if (ownerRel && ::rename(QFile::encodeName(absPath(rel)).constData(), QFile::encodeName(absPath(*ownerRel) + Placeholder::suffix).constData()) == 0) {
                    placeholderPresent.insert(*ownerRel);
                    log(i18n("%1 is an online-only placeholder; it was renamed back to %2", rel, *ownerRel + Placeholder::suffix));
                }
            }
            continue;
        }
        if (!id.isEmpty() && m_items.value(id).dehydrated) {
            // A real file where an online-only one was: it replaces the placeholder and is
            // uploaded like a local change (the server decides on a conflict)
            ItemRow &row = m_items[id];
            QFile::remove(absPath(rel) + Placeholder::suffix);
            row.dehydrated = false;
            row.localSize = -1;
            row.localHash.clear();
            saveRow(row);
        }
        if (id.isEmpty()) {
            if (const QString reason = IgnoreRules::invalidRemoteNameReason(nameOf(rel)); !reason.isEmpty()) {
                markError(rel);
                recordError(i18n("Not uploading %1: the name %2", rel, reason));
                continue;
            }
            if (entry.size <= simpleUploadLimit) {
                m_uploadQueue.append(rel);
            } else {
                uploadNew(rel);
            }
            continue;
        }
        ItemRow &row = m_items[id];
        if (entry.size == row.localSize && entry.mtimeNs == row.localMtimeNs) {
            if (row.localInode != entry.inode) {
                row.localInode = entry.inode;
                saveRow(row);
            }
            continue;
        }
        const QString hash = QuickXorHash::hashFile(absPath(rel));
        if (hash == row.localHash) {
            // Touched but not changed
            storeLocalBaseline(row, rel, hash);
            saveRow(row);
            continue;
        }
        uploadExisting(id, rel);
    }

    runQueuedUploads();

    // Everything we knew that is no longer on disk was deleted locally
    rebuildPathIndex();
    QStringList deleted;
    for (auto it = m_pathToId.constBegin(); it != m_pathToId.constEnd(); ++it) {
        const QString &rel = it.key();
        // Excluded by the user, or a shortcut's, is not the same as deleted: leave the online copy alone
        if (scan.contains(rel) || placeholderPresent.contains(rel) || movedIds.contains(it.value()) || isLeftAlone(rel) || isExcluded(rel)
            || isUnderShortcut(rel) || keepIds.contains(it.value())) {
            continue;
        }
        if (std::any_of(keepFolders.cbegin(), keepFolders.cend(), [&](const QString &folder) {
                return rel == folder || rel.startsWith(folder + QLatin1Char('/'));
            })) {
            continue;
        }
        // The scan may be minutes old by now (uploads ran in between): only what is still gone
        if (LocalScanner::stat(absPath(rel)) || LocalScanner::stat(absPath(rel) + Placeholder::suffix)) {
            continue;
        }
        deleted.append(it.value());
    }
    if (!m_cancel) {
        deleteRemote(deleted);
    }
    m_db->commit();
    return true;
}

bool SyncEngine::createRemoteFolder(const QString &rel, quint64 inode)
{
    const QString parentRel = parentOf(rel);
    const QString parentId = parentRel.isEmpty() ? m_rootItemId : m_pathToId.value(parentRel);
    if (parentId.isEmpty()) {
        markError(rel);
        recordError(i18n("Cannot create %1 online: its parent folder is not synced", rel));
        return false;
    }
    const QString name = nameOf(rel);
    QJsonObject body{{QStringLiteral("name"), name},
                     {QStringLiteral("folder"), QJsonObject()},
                     {QStringLiteral("@microsoft.graph.conflictBehavior"), QStringLiteral("fail")}};
    GraphResponse response = m_graph->postJson(itemPath(parentId) + QStringLiteral("/children"), body);
    if (response.status == 409) {
        // Created online in the meantime: adopt the existing folder
        response = m_graph->get(itemPath(parentId) + QStringLiteral(":/") + encodeSegment(name) + QStringLiteral(":?$select=")
                                + DriveItem::selectFields());
    }
    if (!response.ok()) {
        markError(rel);
        recordError(i18n("Creating folder %1 online failed: %2", rel, response.describe()));
        return false;
    }
    const DriveItem item = DriveItem::fromJson(response.json());
    if (!item.isFolder) {
        markError(rel);
        recordError(i18n("Cannot create folder %1 online: a file with that name exists", rel));
        return false;
    }
    ItemRow row;
    row.id = item.id;
    row.parentId = parentId;
    row.name = item.name;
    row.isFolder = true;
    row.remoteETag = item.eTag;
    row.remoteCTag = item.cTag;
    row.localInode = inode;
    m_items.insert(row.id, row);
    saveRow(row);
    m_pathToId.insert(rel, row.id);
    ++m_stats.foldersCreated;
    log(i18n("Created folder %1 online", rel), QStringLiteral("created"), rel);
    return true;
}

bool SyncEngine::moveRemoteOrKeep(const QString &id, const QString &oldRel, const QString &newRel)
{
    // A name the server refuses: the item stays where it is online, and the user is told
    if (const QString reason = IgnoreRules::invalidRemoteNameReason(nameOf(newRel)); !reason.isEmpty()) {
        markError(newRel);
        recordError(i18n("Cannot move or rename %1 to %2 online: the name %3; it stays as it is online", oldRel, newRel, reason));
        return false;
    }
    if (moveRemote(id, newRel)) {
        return true;
    }
    markError(newRel);
    recordError(i18n("Moving or renaming %1 to %2 online failed; it stays as it is online and is tried again", oldRel, newRel));
    return false;
}

bool SyncEngine::moveRemote(const QString &id, const QString &newRel)
{
    const QString parentRel = parentOf(newRel);
    const QString parentId = parentRel.isEmpty() ? m_rootItemId : m_pathToId.value(parentRel);
    if (parentId.isEmpty()) {
        return false;
    }
    const auto oldRel = relPath(id);
    QJsonObject body{{QStringLiteral("name"), nameOf(newRel)}};
    if (m_items.value(id).parentId != parentId) {
        body.insert(QStringLiteral("parentReference"), QJsonObject{{QStringLiteral("id"), parentId}});
    }
    const GraphResponse response = m_graph->patchJson(itemPath(id), body);
    if (!response.ok()) {
        recordError(i18n("Moving %1 to %2 online failed: %3", oldRel.value_or(id), newRel, response.describe()));
        return false;
    }
    const DriveItem item = DriveItem::fromJson(response.json());
    ItemRow &row = m_items[id];
    row.parentId = parentId;
    row.name = item.name;
    row.remoteETag = item.eTag;
    row.remoteCTag = item.cTag;
    saveRow(row);
    ++m_stats.movedRemote;
    if (oldRel) {
        followPins(*oldRel, newRel);
    }
    log(i18n("Moved %1 to %2 online", oldRel.value_or(id), newRel), QStringLiteral("moved"), newRel);
    return true;
}

std::optional<DriveItem> SyncEngine::uploadContent(const QString &targetPath, const QString &sessionPath, const QString &abs,
                                                   const QByteArray &ifMatch, const QString &conflictBehavior, int *httpStatus)
{
    QFile file(abs);
    if (m_offline) {
        *httpStatus = 0;
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        // A problem with this file here (permissions), not the connection
        *httpStatus = localReadError;
        return std::nullopt;
    }
    HeaderList headers;
    if (!ifMatch.isEmpty()) {
        headers.emplaceBack(QByteArrayLiteral("If-Match"), ifMatch);
    }

    beginTransfers(1, file.size());
    const auto finished = qScopeGuard([this] {
        transferFinished();
    });
    if (file.size() <= simpleUploadLimit) {
        const GraphResponse response =
            m_graph->put(targetPath + QStringLiteral("?@microsoft.graph.conflictBehavior=") + conflictBehavior, file.readAll(), headers);
        *httpStatus = response.status;
        if (!response.ok()) {
            qCDebug(GRAPHSYNC) << "simple upload failed" << response.describe();
            return std::nullopt;
        }
        return DriveItem::fromJson(response.json());
    }

    const qint64 total = file.size();
    const qint64 mtime = QFileInfo(abs).lastModified().toMSecsSinceEpoch();
    QString uploadUrl;
    qint64 offset = 0;

    // A session cut short (lost connection, stopped daemon) is resumed where it ended. Starting
    // over would also clash with the unfinished 0-byte item SharePoint keeps for it, so a new
    // file would end up as a conflict copy of itself
    const QJsonObject stored = uploadSessions().value(sessionPath).toObject();
    if (!stored.isEmpty()) {
        const QString url = stored.value(QLatin1String("url")).toString();
        if (stored.value(QLatin1String("size")).toInteger() == total && stored.value(QLatin1String("mtime")).toInteger() == mtime) {
            const GraphResponse state = m_graph->uploadSessionStatus(url);
            if (state.status == 0) {
                *httpStatus = 0;
                return std::nullopt;
            }
            // nextExpectedRanges: ["12345-"] or ["12345-67890"]
            const QString next = state.json().value(QLatin1String("nextExpectedRanges")).toArray().first().toString();
            bool valid = false;
            const qint64 resumeAt = next.section(QLatin1Char('-'), 0, 0).toLongLong(&valid);
            if (state.ok() && valid && resumeAt >= 0 && resumeAt < total) {
                uploadUrl = url;
                offset = resumeAt;
                qCDebug(GRAPHSYNC) << "resuming upload session at" << offset << "of" << total;
            }
        }
        if (uploadUrl.isEmpty()) {
            m_graph->cancelUploadSession(url);
            storeUploadSession(sessionPath, {});
        }
    }

    if (uploadUrl.isEmpty()) {
        const QJsonObject body{{QStringLiteral("item"), QJsonObject{{QStringLiteral("@microsoft.graph.conflictBehavior"), conflictBehavior}}}};
        const GraphResponse session = m_graph->postJson(sessionPath, body, headers);
        *httpStatus = session.status;
        if (!session.ok()) {
            qCDebug(GRAPHSYNC) << "createUploadSession failed" << session.describe();
            return std::nullopt;
        }
        uploadUrl = session.json().value(QLatin1String("uploadUrl")).toString();
        storeUploadSession(sessionPath,
                           QJsonObject{{QStringLiteral("url"), uploadUrl},
                                       {QStringLiteral("size"), total},
                                       {QStringLiteral("mtime"), mtime},
                                       {QStringLiteral("expires"), session.json().value(QLatin1String("expirationDateTime")).toString()}});
    }

    if (offset > 0) {
        file.seek(offset);
        m_transfer.bytesDone += offset;
    }
    while (offset < total) {
        const QByteArray chunk = file.read(fragmentSizeNow());
        if (chunk.isEmpty()) {
            *httpStatus = localReadError;
            return std::nullopt;
        }
        const GraphResponse fragment = m_graph->uploadFragment(uploadUrl, chunk, offset, total);
        *httpStatus = fragment.status;
        if (!fragment.ok()) {
            qCDebug(GRAPHSYNC) << "fragment upload failed" << fragment.describe();
            // Without a connection the session is kept for the next attempt; any other answer
            // ends it, so no unfinished item stays behind online
            if (fragment.status != 0) {
                m_graph->cancelUploadSession(uploadUrl);
                storeUploadSession(sessionPath, {});
            }
            return std::nullopt;
        }
        offset += chunk.size();
        if (offset >= total) {
            storeUploadSession(sessionPath, {});
            return DriveItem::fromJson(fragment.json());
        }
    }
    return std::nullopt;
}

QJsonObject SyncEngine::uploadSessions() const
{
    return QJsonDocument::fromJson(m_db->meta(QStringLiteral("uploadSessions")).toUtf8()).object();
}

void SyncEngine::storeUploadSession(const QString &sessionPath, const QJsonObject &session)
{
    QJsonObject sessions = uploadSessions();
    // Drop sessions Microsoft has expired by now (files deleted or renamed before a retry)
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = sessions.begin(); it != sessions.end();) {
        const QDateTime expires = QDateTime::fromString(it.value().toObject().value(QLatin1String("expires")).toString(), Qt::ISODateWithMs);
        it = expires.isValid() && expires < now ? sessions.erase(it) : std::next(it);
    }
    if (session.isEmpty()) {
        sessions.remove(sessionPath);
    } else {
        sessions.insert(sessionPath, session);
    }
    m_db->setMeta(QStringLiteral("uploadSessions"), sessions.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(sessions).toJson(QJsonDocument::Compact)));
    // On disk at once: after a crash the upload must be resumed, not started over (which clashes
    // with the unfinished item SharePoint keeps for the old session)
    m_db->commit();
    m_db->begin();
    m_uncommitted = 0;
}

bool SyncEngine::uploadNew(const QString &rel)
{
    const QString parentRel = parentOf(rel);
    const QString parentId = parentRel.isEmpty() ? m_rootItemId : m_pathToId.value(parentRel);
    if (parentId.isEmpty()) {
        markError(rel);
        recordError(i18n("Cannot upload %1: its folder is not synced", rel));
        return false;
    }
    const QString abs = absPath(rel);
    const auto before = LocalScanner::stat(abs);
    const QString hash = QuickXorHash::hashFile(abs);
    const QString target = itemPath(parentId) + QStringLiteral(":/") + encodeSegment(nameOf(rel)) + QLatin1Char(':');

    int status = 0;
    const auto item = uploadContent(target + QStringLiteral("/content"), target + QStringLiteral("/createUploadSession"), abs, {},
                                    QStringLiteral("fail"), &status);
    if (!item) {
        if (status == 409) {
            nameTakenOnline(rel, parentId, hash, before);
            return false;
        }
        uploadFailed(rel, status);
        return false;
    }

    finishUploadNew(rel, parentId, hash, before, *item);
    return true;
}

void SyncEngine::nameTakenOnline(const QString &rel, const QString &parentId, const QString &hash, const std::optional<LocalEntry> &before)
{
    // Our own earlier upload may have arrived while its answer got lost (connection dropped after
    // the last piece): the same content under the same name is that upload, not a conflict
    const GraphResponse existing =
        m_graph->get(itemPath(parentId) + QStringLiteral(":/") + encodeSegment(nameOf(rel)) + QStringLiteral(":?$select=") + DriveItem::selectFields());
    if (existing.ok()) {
        const DriveItem item = DriveItem::fromJson(existing.json());
        if (!item.isFolder && !item.quickXorHash.isEmpty() && item.quickXorHash == hash) {
            qCInfo(GRAPHSYNC).noquote() << QStringLiteral("[%1]").arg(m_root.name) << "already online with the same content:" << rel;
            finishUploadNew(rel, parentId, hash, before, item);
            return;
        }
        if (!item.isFolder && item.size == 0) {
            // An upload that never finished (its session was lost): SharePoint keeps an empty item
            // for it, hidden from folder listings but taking the name. Ours replaces exactly that
            // item (If-Match); replacing an empty file can never lose content.
            int status = 0;
            const auto done = uploadContent(itemPath(item.id) + QStringLiteral("/content"), itemPath(item.id) + QStringLiteral("/createUploadSession"), absPath(rel),
                                            item.eTag.toUtf8(), QStringLiteral("replace"), &status);
            if (done) {
                finishUploadNew(rel, parentId, hash, before, *done);
                return;
            }
            uploadFailed(rel, status);
            return;
        }
    }
    // Same name created online since our last delta: keep ours as a conflict copy, the online
    // file arrives with the next cycle
    makeConflictCopy(rel);
}

void SyncEngine::finishUploadNew(const QString &rel, const QString &parentId, const QString &hash, const std::optional<LocalEntry> &before,
                                 const DriveItem &item)
{
    ItemRow row;
    row.id = item.id;
    row.parentId = parentId;
    row.name = item.name;
    row.remoteETag = item.eTag;
    row.remoteCTag = item.cTag;
    // For Office files this is the hash after property promotion, not ours (phase 0 T9)
    row.remoteHash = item.quickXorHash;
    row.remoteSize = item.size;
    storeLocalBaseline(row, rel, hash);
    if (before && (before->size != row.localSize || before->mtimeNs != row.localMtimeNs)) {
        // Changed while uploading: make the next cycle look again
        row.localSize = -1;
    }
    m_items.insert(row.id, row);
    saveRow(row);
    m_pathToId.insert(rel, row.id);
    ++m_stats.uploaded;
    log(i18n("Uploaded %1", rel), QStringLiteral("uploaded"), rel);
}

void SyncEngine::runQueuedUploads()
{
    const QStringList queue = std::exchange(m_uploadQueue, {});
    setBusy(queue);
    struct NameTaken {
        QString rel;
        QString parentId;
        QString hash;
        std::optional<LocalEntry> before;
    };
    for (qsizetype start = 0; start < queue.size() && !m_cancel; start += transferBatch) {
        QList<ParallelJob> jobs;
        QList<NameTaken> nameTaken;
        for (qsizetype i = start; i < std::min(start + transferBatch, queue.size()); ++i) {
            const QString rel = queue.at(i);
            const QString parentRel = parentOf(rel);
            const QString parentId = parentRel.isEmpty() ? m_rootItemId : m_pathToId.value(parentRel);
            if (parentId.isEmpty()) {
                markError(rel);
                recordError(i18n("Cannot upload %1: its folder is not synced", rel));
                continue;
            }
            QFile file(absPath(rel));
            if (!file.open(QIODevice::ReadOnly)) {
                markError(rel);
                recordError(i18n("Cannot read %1", rel));
                continue;
            }
            const auto before = LocalScanner::stat(absPath(rel));
            ParallelJob job;
            job.verb = "PUT";
            job.pathOrUrl = itemPath(parentId) + QStringLiteral(":/") + encodeSegment(nameOf(rel))
                + QStringLiteral(":/content?@microsoft.graph.conflictBehavior=fail");
            job.body = file.readAll();
            job.headers = {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/octet-stream")}};
            QuickXorHash hasher;
            hasher.addData(job.body);
            const QString hash = hasher.base64Result();
            job.done = [this, rel, parentId, hash, before, &nameTaken](const GraphResponse &response) {
                transferFinished();
                if (response.ok()) {
                    finishUploadNew(rel, parentId, hash, before, DriveItem::fromJson(response.json()));
                } else if (response.status == 409) {
                    nameTaken.append({rel, parentId, hash, before});
                } else if (response.status == 0 && !response.errorString.contains(QLatin1String("cancelled"))) {
                    uploadFailed(rel, 0);
                } else {
                    markError(rel);
                    recordError(i18n("Upload of %1 failed: %2", rel, response.describe()));
                }
            };
            jobs.append(job);
        }
        qint64 batchBytes = 0;
        for (const ParallelJob &job : std::as_const(jobs)) {
            batchBytes += job.body.size();
        }
        beginTransfers(static_cast<int>(jobs.size()), batchBytes);
        m_graph->runParallel(jobs, transferConcurrency);
        // Looked at after the batch: these make requests of their own
        for (const NameTaken &entry : std::as_const(nameTaken)) {
            nameTakenOnline(entry.rel, entry.parentId, entry.hash, entry.before);
        }
    }
}

bool SyncEngine::uploadExisting(const QString &id, const QString &rel)
{
    ItemRow row = m_items.value(id);
    const QString abs = absPath(rel);
    const auto before = LocalScanner::stat(abs);
    const QString hash = QuickXorHash::hashFile(abs);

    int status = 0;
    const auto item = uploadContent(itemPath(id) + QStringLiteral("/content"), itemPath(id) + QStringLiteral("/createUploadSession"), abs,
                                    row.remoteETag.toUtf8(), QStringLiteral("replace"), &status);
    if (!item) {
        if (status == 412) {
            // Changed online and here since the last sync (phase 0 T12): keep both. Without a
            // conflict copy the online version is not fetched over the local work.
            if (!makeConflictCopy(rel).isEmpty()) {
                restoreFromRemote(id);
            }
            return false;
        }
        if (status == 404) {
            // Deleted online after our last delta; it comes back as a new upload next cycle
            m_items.remove(id);
            dropRow(id);
            return false;
        }
        uploadFailed(rel, status);
        return false;
    }
    row.remoteETag = item->eTag;
    row.remoteCTag = item->cTag;
    row.remoteHash = item->quickXorHash;
    row.remoteSize = item->size;
    storeLocalBaseline(row, rel, hash);
    if (before && (before->size != row.localSize || before->mtimeNs != row.localMtimeNs)) {
        row.localSize = -1;
    }
    m_items.insert(id, row);
    saveRow(row);
    ++m_stats.uploaded;
    log(i18n("Uploaded changes to %1", rel), QStringLiteral("uploaded"), rel);
    return true;
}

void SyncEngine::restoreFromRemote(const QString &id)
{
    const GraphResponse response = m_graph->get(itemPath(id) + QStringLiteral("?$select=") + DriveItem::selectFields());
    if (!response.ok()) {
        recordError(i18n("Cannot fetch the online version: %1", response.describe()));
        return;
    }
    const DriveItem item = DriveItem::fromJson(response.json());
    const auto parentRel = relPath(item.parentId);
    if (!parentRel) {
        return;
    }
    download(item, joinRel(*parentRel, item.name));
}

void SyncEngine::deleteRemote(const QStringList &ids)
{
    QList<QPair<int, QString>> ordered;
    for (const QString &id : ids) {
        if (const auto rel = relPath(id)) {
            ordered.append({depthOf(*rel), id});
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });

    for (const auto &[depth, id] : std::as_const(ordered)) {
        Q_UNUSED(depth)
        if (m_cancel) {
            return;
        }
        const ItemRow row = m_items.value(id);
        const QString rel = relPath(id).value_or(row.name);
        if (row.isFolder) {
            // Only delete an online folder that is empty now: anything still inside was added
            // or changed online since our last sync and must not disappear with it
            const GraphResponse children = m_graph->get(itemPath(id) + QStringLiteral("/children?$top=1&$select=id"));
            if (!children.ok() && children.status != 404) {
                // Not known whether it is empty: not deleted now, tried again next cycle
                markError(rel);
                recordError(i18n("Deleting folder %1 online waits: could not check whether it is empty (%2)", rel, children.describe()));
                continue;
            }
            if (children.ok() && !children.json().value(QLatin1String("value")).toArray().isEmpty()) {
                QDir().mkpath(absPath(rel));
                log(i18n("Kept folder %1: it was deleted here, but holds files that changed online", rel));
                m_forceResync = true;
                continue;
            }
        }
        const GraphResponse response =
            m_graph->deleteResource(itemPath(id), row.isFolder ? HeaderList{} : HeaderList{{"If-Match", row.remoteETag.toUtf8()}});
        if (response.ok() || response.status == 404) {
            m_items.remove(id);
            dropRow(id);
            ++m_stats.deletedRemote;
            log(i18n("Deleted %1 online (it went to the site's recycle bin)", rel), QStringLiteral("deletedOnline"), rel);
        } else if (response.status == 412) {
            log(i18n("Restored %1: it was deleted here but changed online", rel));
            restoreFromRemote(id);
        } else {
            markError(rel);
            recordError(i18n("Deleting %1 online failed: %2", rel, response.describe()));
        }
    }
}

// ---------------------------------------------------------------------------------------
// Push endpoint

void SyncEngine::refreshPushEndpoint()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (m_pushExpires.isValid() && now.secsTo(m_pushExpires) > 5 * 60) {
        return;
    }
    if (m_pushRetryAt.isValid() && now < m_pushRetryAt) {
        return;
    }
    // Optional (polling covers it): one attempt. While Microsoft's push service is down it answers
    // 503 with long Retry-After waits, and retrying those held every cycle up for many minutes.
    const GraphResponse response = m_graph->getOnce(QStringLiteral("/drives/%1/root/subscriptions/socketIo").arg(m_root.driveId));
    if (!response.ok()) {
        m_pushRetryAt = now.addSecs(pushRetryMinutes * 60);
        qCInfo(GRAPHSYNC) << "No push endpoint, polling only; trying again in" << pushRetryMinutes << "minutes:" << response.describe();
        return;
    }
    m_pushRetryAt = {};
    const QJsonObject json = response.json();
    m_pushExpires = QDateTime::fromString(json.value(QLatin1String("expirationDateTime")).toString(), Qt::ISODateWithMs);
    if (!m_pushExpires.isValid()) {
        m_pushExpires = now.addSecs(15 * 60);
    }
    Q_EMIT pushEndpointChanged(json.value(QLatin1String("notificationUrl")).toString(), m_pushExpires);
}

}
