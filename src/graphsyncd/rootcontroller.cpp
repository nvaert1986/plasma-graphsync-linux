// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rootcontroller.h"
#include "localwatcher.h"
#include "graphsync_debug.h"
#include "pushclient.h"
#include "syncengine.h"

#include <QDir>

namespace GraphSync
{

namespace
{
// Polling stays on as a safety net even with push: a notification can be missed while the
// socket reconnects, and push only covers online changes
constexpr int pollIntervalMs = 5 * 60 * 1000;
// While offline the next attempt comes sooner, so syncing resumes shortly after the connection
constexpr int offlineRetryMs = 30 * 1000;
constexpr int localDebounceMs = 3000;
constexpr int pushDebounceMs = 1000;
}

RootController::RootController(const RootConfig &root, Auth *auth, QObject *parent)
    : QObject(parent)
    , m_root(root)
{
    m_thread.setObjectName(QStringLiteral("sync-") + root.id);
    m_engine = new SyncEngine(root, auth);
    m_engine->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_engine, &QObject::deleteLater);
    connect(m_engine, &SyncEngine::statusChanged, this, [this](const QVariantMap &status) {
        if (status.value(QStringLiteral("state")) == QLatin1String("error")) {
            const QStringList errors = status.value(QStringLiteral("lastErrors")).toStringList();
            for (const QString &error : errors) {
                // Only a refresh token Microsoft refused (or none at all) means signing in again;
                // "no access token: no connection: ..." is just being offline
                if (error.contains(QLatin1String("sign-in expired")) || error.contains(QLatin1String("not signed in"))) {
                    Q_EMIT signInLost();
                    break;
                }
            }
            if (!errors.isEmpty()) {
                Q_EMIT errorOccurred(errors.constFirst());
            }
        }
        const int interval = status.value(QStringLiteral("state")) == QLatin1String("offline") ? offlineRetryMs : pollIntervalMs;
        if (m_poll.interval() != interval) {
            m_poll.start(interval);
        }
        Q_EMIT statusChanged();
    });
    connect(m_engine, &SyncEngine::activityEntry, this, &RootController::activityEntry);
    connect(m_engine, &SyncEngine::conflictCreated, this, &RootController::conflictCreated);
    connect(m_engine, &SyncEngine::bigDeletePending, this, &RootController::bigDeletePending);
    connect(m_engine, &SyncEngine::persistentError, this, &RootController::errorOccurred);
    connect(m_engine, &SyncEngine::snapshotChanged, this, &RootController::snapshotChanged);
    connect(m_engine, &SyncEngine::shortcutsChanged, this, &RootController::shortcutsChanged);
    connect(m_engine, &SyncEngine::hydrated, this, &RootController::hydrated);
    m_thread.start();

    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        if (!m_paused && m_haltReason.isEmpty()) {
            QMetaObject::invokeMethod(m_engine, &SyncEngine::requestSync, Qt::QueuedConnection);
        }
    });
    m_poll.setInterval(pollIntervalMs);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        scheduleSync(0);
    });
    m_poll.start();

    if (QDir(root.localPath).exists()) {
        m_watcher = new LocalWatcher(root.localPath, this);
        connect(m_watcher, &LocalWatcher::changed, this, [this] {
            scheduleSync(localDebounceMs);
        });
        connect(m_watcher, &LocalWatcher::rootLost, this, [this] {
            // Never let a vanished folder turn into "delete everything online"
            m_haltReason = QStringLiteral("the local folder was removed or moved; syncing stopped");
            m_engine->setPaused(true);
            Q_EMIT statusChanged();
        });
    }

    m_push = new PushClient(this);
    connect(m_engine, &SyncEngine::pushEndpointChanged, m_push, &PushClient::setEndpoint);
    connect(m_push, &PushClient::notificationReceived, this, [this] {
        qCInfo(GRAPHSYNC) << "Change notification for" << m_root.name;
        scheduleSync(pushDebounceMs);
    });
    connect(m_push, &PushClient::endpointLost, this, [this] {
        QMetaObject::invokeMethod(m_engine, &SyncEngine::invalidatePushEndpoint, Qt::QueuedConnection);
        scheduleSync(0);
    });

    scheduleSync(0);
}

RootController::~RootController()
{
    m_push->stop();
    // Abort a running transfer, then let the worker finish its cycle and exit
    m_engine->setPaused(true);
    m_thread.quit();
    m_thread.wait();
}

QString RootController::id() const
{
    return m_root.id;
}

QString RootController::localPath() const
{
    return m_root.localPath;
}

std::shared_ptr<const SyncEngine::StatusSnapshot> RootController::snapshot() const
{
    return m_engine->statusSnapshot();
}

void RootController::scheduleSync(int delayMs)
{
    if (m_paused || !m_haltReason.isEmpty()) {
        return;
    }
    // A pending later sync is pulled forward, never pushed back
    if (!m_debounce.isActive() || m_debounce.remainingTime() > delayMs) {
        m_debounce.start(delayMs);
    }
}

QVariantMap RootController::status() const
{
    QVariantMap status = m_engine->status();
    status.insert(QStringLiteral("pushConnected"), m_push->isConnected());
    status.insert(QStringLiteral("watching"), m_watcher && m_watcher->isActive());
    if (!m_haltReason.isEmpty()) {
        status.insert(QStringLiteral("state"), QStringLiteral("halted"));
        status.insert(QStringLiteral("haltReason"), m_haltReason);
    }
    return status;
}

QVariantList RootController::activity() const
{
    return m_engine->recentActivity();
}

void RootController::syncNow()
{
    scheduleSync(0);
}

void RootController::recheck()
{
    if (!m_haltReason.isEmpty() && QDir(m_root.localPath).exists()) {
        m_haltReason.clear();
        m_engine->setPaused(m_paused);
        Q_EMIT statusChanged();
    }
    QMetaObject::invokeMethod(m_engine, &SyncEngine::checkEverything, Qt::QueuedConnection);
}

void RootController::hydrate(const QString &rel)
{
    // requestHydrate only queues under a mutex and posts to the worker, so it is safe from here
    m_engine->requestHydrate(rel);
}

void RootController::pin(const QString &rel, StateDb::Pin pin)
{
    m_engine->requestPin(rel, pin);
}

void RootController::setExcludedPaths(const QStringList &paths)
{
    QMetaObject::invokeMethod(m_engine, [engine = m_engine, paths] {
        engine->setExcludedPaths(paths);
    }, Qt::QueuedConnection);
}

void RootController::resolveBigDelete(const QString &action)
{
    QMetaObject::invokeMethod(m_engine, [engine = m_engine, action] {
        engine->resolveBigDelete(action);
    }, Qt::QueuedConnection);
}

void RootController::setPaused(bool paused)
{
    m_paused = paused;
    // setPaused only flips atomics and posts a sync, so calling it across threads is safe
    m_engine->setPaused(paused);
    if (!paused) {
        scheduleSync(0);
    }
}

}
