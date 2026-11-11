// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "auth.h"
#include "config.h"
#include "graphclient.h"
#include "itemservice.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>

namespace GraphSync
{

// What the QML pages of the app (setup wizard, Sync button sheet, Choose folders) need.
// Graph calls block with a local event loop, which keeps the window responsive while they run.
class Backend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY accountChanged)
    // The organisation's app registration: set by an administrator (/etc/xdg/graphsyncrc), in the
    // setup wizard or in Settings
    Q_PROPERTY(bool configured READ configured NOTIFY configChanged)
    Q_PROPERTY(QString clientId READ clientId NOTIFY configChanged)
    Q_PROPERTY(QString tenantId READ tenantId NOTIFY configChanged)
    // False on builds that can only sign in with a code (Qt older than 6.9)
    Q_PROPERTY(bool browserSignIn READ browserSignIn CONSTANT)
    Q_PROPERTY(QString displayName READ displayName NOTIFY accountChanged)
    Q_PROPERTY(QString userPrincipalName READ userPrincipalName NOTIFY accountChanged)
    Q_PROPERTY(QString organization READ organization NOTIFY accountChanged)
    Q_PROPERTY(QString baseFolder READ baseFolder WRITE setBaseFolder NOTIFY baseFolderChanged)
    Q_PROPERTY(bool setupDone READ setupDone NOTIFY configChanged)
    // Taking over an install of plasma-onedrive-linux: "" (nothing to do or done), "running",
    // "failed" (importMessage says why; retryImport() tries again)
    Q_PROPERTY(QString importState READ importState NOTIFY importStateChanged)
    Q_PROPERTY(QString importMessage READ importMessage NOTIFY importStateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // Live status of graphsyncd (same JSON as its D-Bus Status method)
    Q_PROPERTY(QVariantMap status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool daemonRunning READ daemonRunning NOTIFY statusChanged)
    Q_PROPERTY(bool pauseOnMeteredNetwork READ pauseOnMeteredNetwork WRITE setPauseOnMeteredNetwork NOTIFY statusChanged)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString stateFolder READ stateFolder CONSTANT)
    // What Copy link creates: "view" or "edit" (the scope is the organisation's default)
    Q_PROPERTY(QString defaultLinkType READ defaultLinkType WRITE setDefaultLinkType NOTIFY defaultLinkTypeChanged)
    // Settings > Advanced; all applied at once (limits) or with the next sync cycle
    Q_PROPERTY(int downloadLimit READ downloadLimit WRITE setDownloadLimit NOTIFY advancedChanged)
    Q_PROPERTY(int uploadLimit READ uploadLimit WRITE setUploadLimit NOTIFY advancedChanged)
    Q_PROPERTY(int minimumFreeSpace READ minimumFreeSpace WRITE setMinimumFreeSpace NOTIFY advancedChanged)
    Q_PROPERTY(QString excludePatterns READ excludePatterns WRITE setExcludePatterns NOTIFY advancedChanged)
    Q_PROPERTY(bool skipDotFiles READ skipDotFiles WRITE setSkipDotFiles NOTIFY advancedChanged)
public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;
    QString importState() const;
    QString importMessage() const;
    Q_INVOKABLE void retryImport();

    bool signedIn() const;
    QString displayName() const;
    QString userPrincipalName() const;
    QString organization() const;
    QString baseFolder() const;
    void setBaseFolder(const QString &folder);
    bool setupDone() const;
    QString lastError() const;
    bool busy() const;
    QVariantMap status() const;
    bool daemonRunning() const;
    bool pauseOnMeteredNetwork() const;
    void setPauseOnMeteredNetwork(bool on);
    QString version() const;
    QString stateFolder() const;
    QString defaultLinkType() const;
    void setDefaultLinkType(const QString &type);
    int downloadLimit() const; // KB/s, 0 = no limit
    void setDownloadLimit(int kbps);
    int uploadLimit() const;
    void setUploadLimit(int kbps);
    int minimumFreeSpace() const; // MB
    void setMinimumFreeSpace(int megabytes);
    QString excludePatterns() const; // one pattern per line
    void setExcludePatterns(const QString &patterns);
    bool skipDotFiles() const;
    void setSkipDotFiles(bool skip);

    // Settings > Advanced > Check All Files Again
    Q_INVOKABLE void recheckAll();

    // Settings window actions, forwarded to graphsyncd
    Q_INVOKABLE void pause(int minutes);
    Q_INVOKABLE void resume();
    Q_INVOKABLE void syncNow();
    Q_INVOKABLE bool removeRoot(const QString &id);
    Q_INVOKABLE void signOut();
    static QString versionCacheFolder();
    // file:// URLs and back, properly encoded ("#", "%" and spaces in names)
    Q_INVOKABLE static QUrl fileUrl(const QString &path);
    Q_INVOKABLE static QString localPath(const QUrl &url);
    static void removeVersionCopies(qint64 olderThanSeconds);
    Q_INVOKABLE void startDaemon();
    // Plasma's panel only finds tray applets that were installed before it started: true when
    // the running plasmashell is older than our installed applet (a package installed during the
    // session), so the tray icon will not show until the panel restarts or the user logs in again
    Q_INVOKABLE bool trayNeedsPanelRestart() const;
    Q_INVOKABLE void restartPanel();
    Q_INVOKABLE QString addPersonal();
    Q_INVOKABLE void openNotificationSettings();
    Q_INVOKABLE void showInFolder(const QString &path);
    // Starts the app with these arguments (System Settings opens the app's pages this way)
    Q_INVOKABLE void launchApp(const QStringList &arguments);

    Q_INVOKABLE bool signIn();
    // Stores the app registration: client is the client ID or a setup text from IT, tenant a
    // work e-mail address, domain or tenant ID (optional when the setup text has one). Changing
    // it while signed in signs out first: the sign-in belongs to the old registration
    Q_INVOKABLE bool saveAppRegistration(const QString &client, const QString &tenant);
    bool configured() const;
    QString clientId() const;
    QString tenantId() const;
    static constexpr bool browserSignIn()
    {
        return Auth::browserLoginAvailable();
    }
    // Sign-in with a code entered on Microsoft's website, for when the browser cannot come back
    // to the app. The code shows up in deviceCode while this waits.
    Q_INVOKABLE bool signInWithCode();
    Q_INVOKABLE void cancelSignIn();
    Q_PROPERTY(QString deviceCode READ deviceCode NOTIFY deviceCodeChanged)
    Q_PROPERTY(QString deviceCodeAddress READ deviceCodeAddress NOTIFY deviceCodeChanged)
    QString deviceCode() const
    {
        return m_deviceCode;
    }
    QString deviceCodeAddress() const
    {
        return m_deviceCodeAddress;
    }

    // Sites and libraries for the picker
    Q_INVOKABLE QVariantList followedSites();
    Q_INVOKABLE QVariantList searchSites(const QString &query);
    Q_INVOKABLE QVariantMap siteFromUrl(const QString &url);
    Q_INVOKABLE QVariantList libraries(const QString &siteId);
    Q_INVOKABLE QVariantMap personalDrive();

    // Choose folders: direct subfolders of a folder ("" = drive root)
    Q_INVOKABLE QVariantList childFolders(const QString &driveId, const QString &itemId);
    Q_INVOKABLE QVariantMap root(const QString &rootId);
    Q_INVOKABLE bool setExcludedFolders(const QString &rootId, const QStringList &paths);

    // The SharePoint Sync button
    Q_INVOKABLE QVariantMap resolveOdopen(const QString &url);

    Q_INVOKABLE QString personalPath() const;
    Q_INVOKABLE QString libraryPath(const QString &siteName, const QString &libraryName) const;
    Q_INVOKABLE bool pathInUse(const QString &path);
    // A folder below the chosen one for a library name (made safe for a path)
    Q_INVOKABLE static QString childPath(const QUrl &folder, const QString &name);
    // True when the folder already holds files (they would be uploaded)
    Q_INVOKABLE static bool folderHasFiles(const QString &path);

    // Share and Version history for a synced file or folder (paths are local)
    Q_INVOKABLE QVariantMap itemInfo(const QString &path);
    Q_INVOKABLE QVariantMap createLink(const QString &path, const QString &type, const QString &scope, int expiryDays);
    Q_INVOKABLE bool copyText(const QString &text);
    Q_INVOKABLE QVariantList permissions(const QString &path);
    Q_INVOKABLE bool removePermission(const QString &path, const QString &permissionId);
    Q_INVOKABLE bool invite(const QString &path, const QStringList &emails, const QString &role, bool notify, const QString &message);
    // {people: [{name, email}], notAllowed}
    Q_INVOKABLE QVariantMap searchPeople(const QString &query);
    Q_INVOKABLE QVariantList versions(const QString &path);
    Q_INVOKABLE bool openVersion(const QString &path, const QString &versionId);
    Q_INVOKABLE bool saveVersionAs(const QString &path, const QString &versionId, const QUrl &target);
    Q_INVOKABLE bool restoreVersion(const QString &path, const QString &versionId);
    // Name of a program that has the file open here (LibreOffice lock file, open file handle), or ""
    Q_INVOKABLE QString openedBy(const QString &path) const;

    // The --copy-link run without a window: link on the clipboard plus a notification. Exit code.
    int copyLink(const QString &path);
    // The --view-online run without a window: opens the item's own web address. Exit code.
    int viewOnline(const QString &path);
    // Opens a Windows internet shortcut (.url, what "Add shortcut to My files" makes for a file)
    // in the browser. Only http and https addresses are followed. Exit code.
    static int openInternetShortcut(const QString &pathOrUrl);
    // Opens a Files On-Demand placeholder (.cloud): the daemon downloads the real file, then it
    // opens in its default program. Exit code.
    int openPlaceholder(const QString &pathOrUrl);
    // "Open With > Download Now" (also in the KDE file dialog): downloads the placeholders'
    // files without opening them. Exit code.
    int downloadPlaceholders(const QStringList &pathsOrUrls);
    // Settings: Files On-Demand for one location
    Q_INVOKABLE void setFilesOnDemand(const QString &rootId, bool on);

    // Register a sync root with graphsyncd (starting it through D-Bus activation if needed)
    Q_INVOKABLE QString addRoot(const QVariantMap &root);
    Q_INVOKABLE void finishSetup();

Q_SIGNALS:
    void configChanged();
    // A second start of the app (single instance) asks this one to show a page
    void activateRequested(const QString &mode, const QString &argument);
    void statusChanged();
    void accountChanged();
    void baseFolderChanged();
    void lastErrorChanged();
    void busyChanged();
    void defaultLinkTypeChanged();
    void advancedChanged();
    void deviceCodeChanged();
    void importStateChanged();
    // The import finished (successfully or not); settings and account were read again
    void importFinished();

private Q_SLOTS:
    void onStatusChanged(const QString &json);

private:
    void startImport();
    void importDone(int result, const QString &message);
    void refreshStatus();
    QString callDaemon(const QString &method, const QVariantList &arguments = {});
    GraphResponse get(const QString &path);
    void setError(const QString &error);
    void setBusy(bool busy);
    void fetchOrganization();
    QVariantList siteList(const QString &path);
    static QString safeName(const QString &name);
    // Resolved once per path and window; sets lastError when it fails
    ItemService::Item item(const QString &path);
    bool sendCommand(const QJsonObject &command);
    void updateGeneral(const std::function<void(GeneralConfig &)> &change);
    // After either kind of sign-in: organisation name, and tell a running graphsyncd
    bool finishSignIn();
    // Asks graphsyncd to download the file behind a placeholder and waits until it is there
    bool hydrateAndWait(const QString &placeholderPath);

    std::unique_ptr<Auth> m_auth;
    QPointer<QThread> m_importThread;
    QString m_importState;
    QString m_importMessage;
    std::unique_ptr<GraphClient> m_graph;
    std::unique_ptr<ItemService> m_itemService;
    QHash<QString, ItemService::Item> m_items;
    QString m_deviceCode;
    QString m_deviceCodeAddress;
    std::atomic_bool m_cancelSignIn = false;
    QString m_baseFolder;
    QString m_lastError;
    bool m_busy = false;
    QVariantMap m_status;
    bool m_daemonRunning = false;
};

}
