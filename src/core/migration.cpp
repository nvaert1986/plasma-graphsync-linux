// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "migration.h"
#include "auth.h"
#include "config.h"
#include "graphsync_debug.h"

#include <KConfig>
#include <KConfigGroup>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include <unistd.h>

namespace GraphSync::Migration
{

namespace
{
const QString legacyService = QStringLiteral("io.github.nvaert1986.Podl");
const QString legacyUnit = QStringLiteral("plasma-onedrive-linux.service");
const QString legacyDesktopId = QStringLiteral("io.github.nvaert1986.plasma-onedrive-linux");

QString configPath(const QString &name)
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath(name);
}

QString markerPath()
{
    return QDir(Config::stateDirectory()).filePath(QStringLiteral("imported-from-plasma-onedrive-linux"));
}

void writeMarker()
{
    QFile marker(markerPath());
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        marker.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8() + '\n');
    }
}

// graphsyncrc with a finished setup: imported by 0.7.0/0.7.1 (before the marker existed), or set
// up anew; either way there is nothing to take over any more
bool newSetupExists()
{
    const QString path = configPath(QStringLiteral("graphsyncrc"));
    return QFileInfo::exists(path) && KConfig(path, KConfig::SimpleConfig).group(QStringLiteral("General")).readEntry("SetupDone", false);
}

// True when the old daemon is gone from the session bus
bool stopLegacyDaemon()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected() && bus.interface()->isServiceRegistered(legacyService)) {
        QDBusMessage quit = QDBusMessage::createMethodCall(legacyService, QStringLiteral("/Podl"), legacyService, QStringLiteral("Command"));
        quit << QStringLiteral(R"({"command":"quit"})");
        bus.call(quit, QDBus::Block, 10000);
    }
    // Never start it again. Masked, not only disabled: its D-Bus service file names the unit, so a
    // call to the old name from a leftover tray widget or Dolphin plugin would start it otherwise.
    // Disabled while still running (no window for a restart), then masked and stopped without
    // waiting for it here (the loop below waits, bounded)
    QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("disable"), legacyUnit});
    QProcess::execute(QStringLiteral("systemctl"),
                      {QStringLiteral("--user"), QStringLiteral("mask"), QStringLiteral("--now"), QStringLiteral("--no-block"), legacyUnit});
    QElapsedTimer waited;
    waited.start();
    while (bus.isConnected() && bus.interface()->isServiceRegistered(legacyService) && waited.elapsed() < 20000) {
        QThread::msleep(200);
    }
    return !(bus.isConnected() && bus.interface()->isServiceRegistered(legacyService));
}

bool copyDirectory(const QString &from, const QString &to)
{
    if (!QDir().mkpath(to)) {
        return false;
    }
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    bool ok = true;
    while (it.hasNext()) {
        const QString source = it.next();
        const QString target = QDir(to).filePath(it.fileName());
        QFile::remove(target);
        ok = QFile::copy(source, target) && ok;
        QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner);
    }
    return ok;
}

// Sync button (odopen://), .url shortcuts and placeholders opened with the old app, in every file
// where desktops keep default applications; each one replaced atomically
void repointDefaultHandlers()
{
    const QStringList files{configPath(QStringLiteral("mimeapps.list")),
                            configPath(QStringLiteral("kde-mimeapps.list")),
                            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/applications/mimeapps.list")};
    for (const QString &path : files) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray text = file.readAll();
        file.close();
        const QByteArray updated = QByteArray(text).replace(legacyDesktopId.toUtf8(), QByteArrayLiteral("io.github.nvaert1986.graphsync"));
        if (updated == text) {
            continue;
        }
        QSaveFile save(path);
        if (save.open(QIODevice::WriteOnly)) {
            save.write(updated);
            save.commit();
        }
    }
}

QString lockPath()
{
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!runtime.isEmpty()) {
        return QDir(runtime).filePath(QStringLiteral("graphsync-migration.lock"));
    }
    // No runtime folder: a name of our own in the shared temp folder
    return QDir(QDir::tempPath()).filePath(QStringLiteral("graphsync-migration-%1.lock").arg(getuid()));
}
}

void adoptSharedAccounts(int lockTimeoutMs)
{
    const QString path = configPath(QStringLiteral("graphsyncrc"));
    if (!KConfig(path, KConfig::SimpleConfig).hasGroup(QStringLiteral("Account"))) {
        return;
    }
    // The daemon, graphsyncctl (also the "migrate" autostart) and the app may all start at the
    // first login after the upgrade: one moves the account, the others then find nothing to do
    // (without the lock both could add the user to graphaccountsrc under two ids)
    QLockFile lock(QDir(QFileInfo(lockPath()).absolutePath()).filePath(QStringLiteral("graphsync-accounts.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(lockTimeoutMs)) {
        qCWarning(GRAPHSYNC) << "Another process is moving the account to" << GraphCore::Accounts::configFileName() << "- not waiting longer";
        return;
    }
    KConfig file(path, KConfig::SimpleConfig);
    if (!file.hasGroup(QStringLiteral("Account"))) {
        return;
    }
    const KConfigGroup old = file.group(QStringLiteral("Account"));
    AccountInfo account;
    account.objectId = old.readEntry("ObjectId", QString()).toLower();
    account.tenantId = old.readEntry("TenantId", QString()).toLower();
    account.userPrincipalName = old.readEntry("UserPrincipalName", QString());
    account.displayName = old.readEntry("DisplayName", QString());
    account.organization = old.readEntry("Organization", QString());
    // The registration it signed in with (the cascade: an administrator's one counts too)
    account.clientId = Config::auth().clientId;
    if (account.tenantId.isEmpty()) {
        account.tenantId = Config::auth().tenantId;
    }
    // Signed out: nothing to move; the next sign-in registers the account
    if (!account.objectId.isEmpty() && !account.tenantId.isEmpty()) {
        // Registered as if just signed in: current account, owner of the locations without one
        const AccountInfo stored = Config::registerAccount(account, true);
        // Its sign-in is still under the registration's key as 0.7 wrote it (any spelling); Auth
        // moves it on first use
        const AuthConfig written = Config::auth();
        Config::setLegacyTokenUser(stored.objectId, Auth::keychainKeyFor({written.clientId, written.tenantId, {}}));
        qCInfo(GRAPHSYNC).noquote() << "Moved the account" << stored.userPrincipalName << "to" << GraphCore::Accounts::configFileName() << "as" << stored.id;
    }
    // Last, so an interrupted run is simply done again (KConfig writes only this change)
    file.deleteGroup(QStringLiteral("Account"));
    file.sync();
}

bool pending()
{
    return QFileInfo::exists(configPath(QStringLiteral("podlrc"))) && !QFileInfo::exists(markerPath()) && !newSetupExists();
}

Result importLegacyInstall(QString *message, int lockTimeoutMs)
{
    const QString newConfig = configPath(QStringLiteral("graphsyncrc"));
    const QString oldConfig = configPath(QStringLiteral("podlrc"));
    if (!QFileInfo::exists(oldConfig) || QFileInfo::exists(markerPath())) {
        return Result::NotNeeded;
    }
    if (newSetupExists()) {
        writeMarker();
        return Result::NotNeeded;
    }

    QLockFile lock(lockPath());
    // Never stale by age: stopping the old daemon can take a while, and a second process must not
    // start copying meanwhile. A crashed holder is still detected (its process is gone)
    lock.setStaleLockTime(0);
    if (!lock.tryLock(lockTimeoutMs)) {
        *message = QStringLiteral("another process is importing the plasma-onedrive-linux settings");
        return Result::Busy;
    }
    if (QFileInfo::exists(markerPath())) {
        // Done by another process while we waited
        return Result::NotNeeded;
    }

    qCInfo(GRAPHSYNC) << "Taking over the settings of plasma-onedrive-linux";
    if (!stopLegacyDaemon()) {
        // Its databases may be mid-write: copying them now could give a broken copy
        *message = QStringLiteral("the plasma-onedrive-linux daemon did not stop yet");
        return Result::Busy;
    }

    const QString stateBase = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    const QString oldState = QDir(stateBase).filePath(QStringLiteral("podl"));
    if (QFileInfo::exists(oldState) && !copyDirectory(oldState, Config::stateDirectory())) {
        *message = QStringLiteral("could not copy the sync state from %1 to %2").arg(oldState, Config::stateDirectory());
        return Result::Failed;
    }

    // The IDs as the old client saw them: the user's file over the system ones in every
    // XDG_CONFIG_DIRS entry, field by field (an administrator's podlrc is not ours to move)
    const KConfig cascade(QStringLiteral("podlrc"));
    const KConfigGroup cascadeAuth = cascade.group(QStringLiteral("Auth"));
    const AuthConfig legacyAuth{cascadeAuth.readEntry("ClientId", QString()), cascadeAuth.readEntry("TenantId", QString()), {}};
    // Stored normalised from now on; a domain as tenant becomes its tenant ID when it resolves
    AuthConfig auth = Auth::normalized(legacyAuth);
    if (!auth.tenantId.isEmpty() && !Auth::isGuid(auth.tenantId)) {
        bool offline = false;
        const QString resolved = Auth::resolveTenant(auth.tenantId, &offline);
        if (!resolved.isEmpty()) {
            auth.tenantId = resolved;
        }
    }
    QString tokenError;
    if (!legacyAuth.clientId.isEmpty() && !Auth::importLegacyToken(legacyAuth, auth, &tokenError)) {
        // Not fatal: the user signs in again
        qCWarning(GRAPHSYNC) << "The sign-in of plasma-onedrive-linux was not taken over:" << tokenError;
    }

    repointDefaultHandlers();

    // Settings written while the import was pending (no finished setup) give way to the old setup
    if (QFileInfo::exists(newConfig)) {
        const QString backup = newConfig + QStringLiteral(".before-import");
        QFile::remove(backup);
        QFile::rename(newConfig, backup);
    }
    if (!QFile::copy(oldConfig, newConfig)) {
        *message = QStringLiteral("could not copy %1 to %2").arg(oldConfig, newConfig);
        return Result::Failed;
    }
    {
        KConfig imported(newConfig, KConfig::SimpleConfig);
        KConfigGroup group = imported.group(QStringLiteral("Auth"));
        if (!auth.clientId.isEmpty()) {
            group.writeEntry("ClientId", auth.clientId);
        }
        if (!auth.tenantId.isEmpty()) {
            group.writeEntry("TenantId", auth.tenantId);
        }
    }
    if (KConfig(oldConfig, KConfig::SimpleConfig).group(QStringLiteral("General")).readEntry("SetupDone", false)) {
        QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("enable"), QStringLiteral("plasma-graphsync-linux.service")});
    }
    // Last: the import is done
    writeMarker();
    *message = QStringLiteral("took over the settings, sync state and sign-in of plasma-onedrive-linux");
    qCInfo(GRAPHSYNC).noquote() << "Migration:" << *message;
    return Result::Imported;
}

}
