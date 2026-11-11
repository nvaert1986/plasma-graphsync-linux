// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config.h"
#include "driveitem.h"
#include "ignorerules.h"
#include "localscanner.h"
#include "statedb.h"

#include <QMutex>

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <optional>

namespace GraphSync
{

class Auth;
class GraphClient;
struct GraphResponse;

// Two-way sync of one root (a library or a folder in it) with a local folder.
// syncOnce() blocks; in the daemon the engine lives in its own worker thread and is driven
// through requestSync(), which coalesces requests that arrive while a cycle is running.
class SyncEngine : public QObject
{
    Q_OBJECT
public:
    enum class Mode {
        TwoWay,
        DownloadOnly,
    };

    struct Stats {
        int downloaded = 0;
        int uploaded = 0;
        int movedLocal = 0;
        int movedRemote = 0;
        int deletedLocal = 0;
        int deletedRemote = 0;
        int foldersCreated = 0;
        int conflicts = 0;
        int deltaPages = 0;
        int deltaItems = 0;
    };

    SyncEngine(const RootConfig &root, Auth *auth, QObject *parent = nullptr);
    ~SyncEngine() override;

    void setMode(Mode mode);

    // Runs one full cycle. Returns false on an error that stopped the cycle.
    bool syncOnce(QString *error = nullptr);

    // What Dolphin's emblems need (phase 3): every known item's local baseline, what is being
    // transferred right now and what failed in the last cycle. Published by the sync thread,
    // read by the daemon's socket server in the main thread.
    struct StatusSnapshot {
        struct Entry {
            qint64 size = -1;
            qint64 mtimeNs = 0;
            bool isFolder = false;
        };
        QHash<QString, Entry> known; // relative path -> local baseline
        QSet<QString> busy;
        QSet<QString> errors;
        QStringList excluded;
        // The user's own exclusions (patterns, dot-files; .nosync is checked on disk)
        IgnoreRules::UserRules userRules;
        // Files On-Demand: online-only files (relative paths of the real files)
        QSet<QString> dehydrated;
    };
    std::shared_ptr<const StatusSnapshot> statusSnapshot() const;

    // "Add shortcut to My files" folders in this root. This engine leaves their contents alone;
    // the daemon syncs each one as a root of its own on the drive it points to.
    struct Shortcut {
        QString id; // the shortcut item in this drive
        QString name;
        QString parentId;
        QString driveId; // what it points to
        QString remoteId;
        QString webUrl;
        quint64 inode = 0; // of the local folder, to recognise a rename here
    };

    // Thread-safe snapshots for the daemon's D-Bus interface
    QVariantMap status() const;
    QVariantList recentActivity() const;
    Stats lastStats() const;

public Q_SLOTS:
    void requestSync();
    void setPaused(bool paused);
    // Choose folders: relative paths that are no longer synced locally
    void setExcludedPaths(const QStringList &paths);
    // Answer to the mass-delete question: "delete" (apply online once) or "restore"
    void resolveBigDelete(const QString &action);
    // Called when the push endpoint is gone (socket closed): fetch a new one on the next cycle
    void invalidatePushEndpoint();
    // Troubleshooting: compare everything again (full listing, forget retries and given-up items)
    void checkEverything();
    // Files On-Demand requests; thread-safe, handled at once when idle, otherwise between two
    // batches of the running cycle. rel is the real file's path (without .cloud).
    void requestHydrate(const QString &rel);
    void requestPin(const QString &rel, StateDb::Pin pin);

Q_SIGNALS:
    void statusChanged(const QVariantMap &status);
    void activity(const QString &message);
    // Structured activity entry (time, message, action, path, name, icon, root)
    void activityEntry(const QVariantMap &entry);
    void conflictCreated(const QString &path, const QString &copyName);
    void bigDeletePending(int missing, int known);
    void persistentError(const QString &message);
    // A new status snapshot is available (emblems need refreshing)
    void snapshotChanged();
    void pushEndpointChanged(const QString &notificationUrl, const QDateTime &expires);
    // A requested download of an online-only file is done (or failed)
    void hydrated(const QString &rel, bool ok, const QString &error);
    // After every cycle: [{id, name, rel, driveId, remoteId, webUrl}] for the shortcuts that
    // exist, and the relative paths of removed ones whose local folder is still there
    void shortcutsChanged(const QVariantList &shortcuts, const QStringList &removedPaths);

private:
    bool ensureInitialized(QString *error);
    bool checkLocalRoot(QString *error);
    void removeStaleTempFiles();

    // Remote -> local
    bool remotePhase(QString *error);
    void applyRemoteItem(const DriveItem &item, bool incremental);
    void enumerateNewFolder(const QString &folderId);
    void applyRemoteDeletes(const QStringList &ids);
    void deleteLocalSubtree(const QString &id, bool becauseExcluded = false);
    // Pins are kept by path: they follow moves, and pins of vanished paths are removed
    void followPins(const QString &fromRel, const QString &toRel);
    void pruneStalePins();
    // Shared by the deletes of one batch: the child index is built once, and a folder's check
    // (scan, and what is new, changed or special in it) serves its subfolders too
    struct DeleteContext {
        QMultiHash<QString, QString> children;
        QHash<QString, LocalEntry> scanned;
        QStringList dirty;
    };
    DeleteContext deleteContext() const;
    void deleteLocalSubtree(const QString &id, bool becauseExcluded, DeleteContext &context, bool checked);
    void checkSubtree(const QString &id, const QString &rel, DeleteContext &context) const;
    // Moves a folder whose contents are all synced and unchanged to the Trash as one entry
    bool trashWholeFolder(const QString &id, const QString &rel, bool becauseExcluded, const DeleteContext &context);
    bool isExcluded(const QString &rel) const;
    void purgeExcluded();
    QList<DriveItem> fetchRetryItems(QHash<QString, int> *attempts);
    void fetchQuota();
    bool download(const DriveItem &item, const QString &rel);
    // With a download limit: the file in paced pieces (HTTP Range)
    bool downloadInPieces(const DriveItem &item, const QString &rel);
    bool finishDownload(const DriveItem &item, const QString &rel, const QString &tmp, const GraphResponse &response);
    void runQueuedDownloads();
    QString tempPathFor(const DriveItem &item, const QString &rel) const;

    // Local -> remote
    bool localPhase(QString *error);
    void rebuildPathIndex();
    bool createRemoteFolder(const QString &rel, quint64 inode);
    bool moveRemote(const QString &id, const QString &newRel);
    bool uploadNew(const QString &rel);
    void finishUploadNew(const QString &rel, const QString &parentId, const QString &hash, const std::optional<LocalEntry> &before,
                         const DriveItem &item);
    void runQueuedUploads();
    // A new upload found its name taken online: adopt the online file when it is our own upload
    // whose answer got lost (same content), else keep ours as a conflict copy
    void nameTakenOnline(const QString &rel, const QString &parentId, const QString &hash, const std::optional<LocalEntry> &before);
    bool uploadExisting(const QString &id, const QString &rel);
    std::optional<DriveItem> uploadContent(const QString &targetPath, const QString &sessionPath, const QString &abs, const QByteArray &ifMatch,
                                           const QString &conflictBehavior, int *httpStatus);
    // Upload sessions cut short, kept in the state DB to resume them (key: the session path)
    QJsonObject uploadSessions() const;
    void storeUploadSession(const QString &sessionPath, const QJsonObject &session);
    void deleteRemote(const QStringList &ids);
    void restoreFromRemote(const QString &id);

    // Never synced in either direction: the built-in ignore rules (temp, lock, desktop files, *.cloud)
    // and the user's exclusions (patterns, dot-files, folders holding a .nosync file)
    bool isLeftAlone(const QString &rel);

    // Files On-Demand
    StateDb::Pin effectivePin(const QString &rel, bool *explicitPin) const;
    bool createPlaceholder(const DriveItem &item, const QString &rel);
    bool dehydrate(const QString &id, const QString &rel);
    void applyPins();
    Q_INVOKABLE void processRequests();
    bool hydrateNow(const QString &rel, QString *error);
    // Transfer progress for the tray (files and bytes of the current batch of transfers)
    void beginTransfers(int files, qint64 bytes);
    void transferFinished();
    void publishTransfer(bool force);
    qint64 fragmentSizeNow() const;

    // Shortcuts
    void loadShortcuts();
    void saveShortcuts();
    std::optional<QString> shortcutRel(const Shortcut &shortcut) const;
    QSet<QString> shortcutPaths() const;
    bool isUnderShortcut(const QString &rel) const;
    void applyRemoteShortcut(const DriveItem &item);
    void forgetShortcut(const QString &id, const QString &reason);
    void handleLocalShortcutChanges(const QHash<QString, LocalEntry> &scan);
    void emitShortcuts();

    // Helpers
    std::optional<QString> relPath(const QString &id) const;
    QString absPath(const QString &rel) const;
    bool localChangedSinceBaseline(const ItemRow &row, const LocalEntry &entry, const QString &rel) const;
    QString makeConflictCopy(const QString &rel);
    bool moveLocal(const QString &fromRel, const QString &toRel);
    void storeLocalBaseline(ItemRow &row, const QString &rel, const QString &hash);
    QString itemPath(const QString &id) const;
    void saveRow(const ItemRow &row);
    void dropRow(const QString &id);

    void log(const QString &message, const QString &action = {}, const QString &rel = {}, const QString &by = {});
    void markError(const QString &rel);
    void setBusy(const QStringList &rels);
    // rebuildKnown re-reads every item's baseline; otherwise only busy/errors are refreshed
    void publishSnapshot(bool rebuildKnown);
    void recordError(const QString &message);
    // An upload failed: without a connection the cycle counts as offline, else a sync problem
    void uploadFailed(const QString &rel, int status);
    void setState(const QString &state);
    void refreshPushEndpoint();

    RootConfig m_root;
    Auth *m_auth;
    Mode m_mode = Mode::TwoWay;
    std::unique_ptr<GraphClient> m_graph;
    std::unique_ptr<StateDb> m_db;

    // State of the current cycle
    QHash<QString, ItemRow> m_items;
    QHash<QString, QString> m_pathToId;
    QString m_rootItemId;
    QStringList m_cycleErrors;
    QSet<QString> m_seenRemote;
    struct QueuedDownload {
        DriveItem item;
        QString rel;
    };
    QList<QueuedDownload> m_downloadQueue;
    QStringList m_uploadQueue;
    int m_uncommitted = 0;
    QSet<QString> m_failedIds;
    QSet<QString> m_errorPaths;
    QSet<QString> m_busyPaths;
    QHash<QString, StatusSnapshot::Entry> m_knownCache;
    QSet<QString> m_dehydratedCache;
    std::shared_ptr<const StatusSnapshot> m_snapshot;
    bool m_excludedDirty = false;
    // Settings of the current cycle (read at its start)
    GeneralConfig m_settings;
    IgnoreRules::UserRules m_userRules;
    QHash<QString, bool> m_noSyncCache;
    QString m_selfId;
    // Not enough disk space: the cycle stops before the delta position moves on
    bool m_lowSpace = false;
    // The cycle could not reach Microsoft 365: state "offline", no error notification
    bool m_offline = false;
    QString m_lowSpaceMessage;
    struct Transfer {
        int files = 0;
        int filesDone = 0;
        qint64 bytes = 0;
        qint64 bytesDone = 0;
    } m_transfer;
    QElapsedTimer m_transferPublished;
    QHash<QString, StateDb::Pin> m_pins;
    bool m_hasDehydrated = false;
    QMutex m_requestMutex;
    QStringList m_hydrateRequests;
    QList<QPair<QString, StateDb::Pin>> m_pinRequests;
    QHash<QString, Shortcut> m_shortcuts;
    // Relative paths of removed shortcuts whose folder the daemon has yet to move to the Trash;
    // never uploaded in the meantime
    QStringList m_detachedShortcuts;
    // 0 = ask, 1 = delete online once, 2 = restore missing items
    int m_bigDeleteDecision = 0;
    bool m_forceResync = false;
    Stats m_stats;

    // Coalescing and control
    bool m_running = false;
    bool m_pending = false;
    std::atomic_bool m_paused = false;
    std::atomic_bool m_cancel = false;

    QDateTime m_pushExpires;

    mutable QMutex m_statusMutex;
    QVariantMap m_status;
    QVariantList m_activity;
    Stats m_lastStats;
};

}
