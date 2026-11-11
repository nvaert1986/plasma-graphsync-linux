// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "auth.h"
#include "config.h"
#include "ratelimiter.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <memory>

namespace GraphSync
{

class RootController;

// Owns the account, the sync roots and the global pause state, and raises notifications.
// Runs while signed out too, so the tray applet can offer "Sign in".
class Daemon : public QObject
{
    Q_OBJECT
public:
    explicit Daemon(QObject *parent = nullptr);
    ~Daemon() override;

    QJsonObject status() const;
    QList<RootController *> controllers() const
    {
        return m_roots.values();
    }

    // Copy link: the daemon owns the clipboard, because on Wayland the copied text lives in the
    // process that set it, and the app's --copy-link run exits right away
    bool copyToClipboard(const QString &text);

    // Files On-Demand. Paths may be the placeholder (.cloud) or the real file.
    // hydrate() returns false when the path is not in a synced folder.
    bool hydrate(const QString &path);
    // "pending", "done", "failed: <reason>" or "" when unknown
    QString hydrationState(const QString &path) const;
    void pin(const QStringList &paths, const QString &state);
    void notifyLinkCopied(const QString &path, const QString &name, const QString &scope, const QString &type);
    void notifyLinkFailed(const QString &name, const QString &error);
    QJsonArray activity() const;

    void syncNow();
    void pause(int minutes);
    void resume();
    void reloadAccount();
    void signOut();
    QString addRoot(const RootConfig &root);
    bool removeRoot(const QString &id);
    void setExcludedFolders(const QString &id, const QStringList &paths);
    void resolveBigDelete(const QString &id, const QString &action);
    // Settings changed in System Settings (pause on metered networks)
    void reloadSettings()
    {
        updateMetered();
        // Bandwidth limits apply to running transfers at once; the rest at the next cycle
        const GeneralConfig general = Config::general();
        RateLimiter::setLimits(qint64(general.downloadLimitKBps) * 1024, qint64(general.uploadLimitKBps) * 1024);
        Q_EMIT statusChanged();
    }

Q_SIGNALS:
    void statusChanged();
    // Roots were added, removed, started or stopped
    void rootsChanged();
    void activityAdded(const QVariantMap &entry);

private:
    void startRoots();
    void stopRoots();
    RootController *createController(const RootConfig &root);
    void applyPause();
    // The root holding path (longest match) and the path below it
    RootController *controllerFor(const QString &path, QString *rel) const;
    QHash<QString, QString> m_hydrations;
    // Keeps the shortcut roots of one root in line with the shortcuts its engine found
    void reconcileShortcuts(const QString &parentId, const QVariantList &shortcuts, const QStringList &removedPaths);
    void updateMetered();

    void notifyConflict(const QString &path, const QString &copyName);
    void notifyError(const QString &root, const QString &message);
    void notifyBigDelete(const QString &rootId, const QString &rootName, int missing, int known);
    void notifySignedOut();
    static void launchApp(const QStringList &arguments);

    std::unique_ptr<Auth> m_auth;
    QHash<QString, RootController *> m_roots;
    bool m_signedIn = false;

    bool m_userPaused = false;
    QDateTime m_pausedUntil;
    QTimer m_resumeTimer;
    bool m_meteredPaused = false;

    // Errors already notified, with the time, so a recurring error is shown once an hour
    QHash<QString, QDateTime> m_notifiedErrors;
    bool m_signedOutNotified = false;
};

}
