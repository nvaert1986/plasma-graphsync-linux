// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config.h"
#include "syncengine.h"

#include <QObject>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

namespace GraphSync
{

class Auth;
class LocalWatcher;
class PushClient;
class SyncEngine;

// Drives one sync root in the daemon: the engine runs in its own thread; the file watcher,
// push client and timers live in the main thread and only ever ask the engine to sync.
class RootController : public QObject
{
    Q_OBJECT
public:
    RootController(const RootConfig &root, Auth *auth, QObject *parent = nullptr);
    ~RootController() override;

    QString id() const;
    QString localPath() const;
    std::shared_ptr<const SyncEngine::StatusSnapshot> snapshot() const;
    QVariantMap status() const;
    QVariantList activity() const;

    void syncNow();
    // Troubleshooting: compare everything again; also lifts a stop because the folder was gone,
    // once it is back
    void recheck();
    // Files On-Demand; rel is the real file's path below the root
    void hydrate(const QString &rel);
    void pin(const QString &rel, StateDb::Pin pin);
    void setPaused(bool paused);
    void setExcludedPaths(const QStringList &paths);
    void resolveBigDelete(const QString &action);

Q_SIGNALS:
    void statusChanged();
    void activityEntry(const QVariantMap &entry);
    void conflictCreated(const QString &path, const QString &copyName);
    void bigDeletePending(int missing, int known);
    void errorOccurred(const QString &message);
    void signInLost();
    // Emblems in Dolphin need refreshing
    void snapshotChanged();
    // The shortcuts in this root, after every cycle (see SyncEngine::shortcutsChanged)
    void shortcutsChanged(const QVariantList &shortcuts, const QStringList &removedPaths);
    void hydrated(const QString &rel, bool ok, const QString &error);

private:
    void scheduleSync(int delayMs);

    RootConfig m_root;
    QThread m_thread;
    SyncEngine *m_engine = nullptr;
    LocalWatcher *m_watcher = nullptr;
    PushClient *m_push = nullptr;
    QTimer m_debounce;
    QTimer m_poll;
    bool m_paused = false;
    QString m_haltReason;
};

}
