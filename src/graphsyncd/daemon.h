// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "auth.h"
#include "config.h"
#include "ratelimiter.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <memory>

namespace GraphSync
{

class RootController;

// Owns the accounts' sign-ins, the sync roots and the global pause state, and raises
// notifications. Every root syncs with its own account; an account that is signed out stops only
// its own roots. Runs while signed out too, so the tray applet can offer "Sign in".
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
    // After one account signed in: only its locations restart
    void reloadOneAccount(const QString &accountId);
    // Signs an account out of this computer (empty: the current one); the others keep syncing
    void signOut(const QString &accountId = {});
    // Signs the account out and stops all its locations (the local files stay). The account stays
    // in graphaccountsrc: other apps of the suite may use it
    void removeAccount(const QString &accountId);
    // The account the app works with (new locations, sign-in, the Sync button)
    bool setCurrentAccount(const QString &accountId);
    QString addRoot(const RootConfig &root);
    bool removeRoot(const QString &id);
    void setExcludedFolders(const QString &id, const QStringList &paths);
    void resolveBigDelete(const QString &id, const QString &action);
    // Settings changed in System Settings (pause on metered networks)
    void reloadSettings()
    {
        invalidateStatusCache();
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
    RootController *createController(const RootConfig &root, std::shared_ptr<Auth> auth);
    void applyPause();
    // What status() shows of graphsyncrc and graphaccountsrc, reread at most every two seconds
    void invalidateStatusCache();
    void refreshStatusCache() const;
    mutable QElapsedTimer m_statusCacheAge;
    mutable AccountInfo m_cachedCurrent;
    mutable QList<RootConfig> m_cachedRoots;
    mutable QList<AccountInfo> m_cachedAccounts;
    mutable GeneralConfig m_cachedGeneral;
    // The root holding path (longest match) and the path below it
    RootController *controllerFor(const QString &path, QString *rel) const;
    QHash<QString, QString> m_hydrations;
    // Keeps the shortcut roots of one root in line with the shortcuts its engine found
    void reconcileShortcuts(const QString &parentId, const QVariantList &shortcuts, const QStringList &removedPaths);
    void updateMetered();

    void notifyConflict(const QString &path, const QString &copyName);
    void notifyError(const QString &root, const QString &message);
    void notifyBigDelete(const QString &rootId, const QString &rootName, int missing, int known);
    void notifySignedOut(const QString &accountId);
    static void launchApp(const QStringList &arguments);
    // The sign-in of an account (created on first use); null for an unknown account. Shared:
    // reading KWallet runs a local event loop, in which another D-Bus call may reload or sign out
    // and drop it from m_auths; whoever holds it keeps it alive until done
    std::shared_ptr<Auth> authFor(const QString &accountId);
    // Roots stop before the sign-ins they use go
    void dropAccounts();

    // Account id -> its sign-in; the root controllers share them
    QHash<QString, std::shared_ptr<Auth>> m_auths;
    // A reload asked for while one runs (from inside its KWallet wait) runs right after it
    bool m_reloading = false;
    bool m_reloadPending = false;
    void reloadOnce();
    QHash<QString, RootController *> m_roots;
    // Root id -> account id
    QHash<QString, QString> m_rootAccounts;
    // The current account (the one the app works with) is signed in
    bool m_signedIn = false;
    // Account id -> has a sign-in here (a refresh token in KWallet that still works); read once per
    // reload, so status() never waits for KWallet
    QHash<QString, bool> m_accountSignedIn;
    // After a sign-out or removal of the current account: another signed-in one takes over
    void chooseCurrentAccount();

    bool m_userPaused = false;
    QDateTime m_pausedUntil;
    QTimer m_resumeTimer;
    bool m_meteredPaused = false;

    // Errors already notified, with the time, so a recurring error is shown once an hour
    QHash<QString, QDateTime> m_notifiedErrors;
    // Accounts already told to sign in again
    QSet<QString> m_signedOutNotified;
};

}
