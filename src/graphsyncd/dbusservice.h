// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QStringList>

namespace GraphSync
{

class Daemon;

// Session-bus interface of graphsyncd at io.github.nvaert1986.GraphSync /GraphSync. Results are JSON strings so
// graphsyncctl, the tray applet (QML), the settings page and scripts can read them without custom
// D-Bus types. Command() takes a JSON object for callers that can only pass strings easily.
class DBusService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.nvaert1986.GraphSync")
public:
    explicit DBusService(Daemon *daemon, QObject *parent = nullptr);

public Q_SLOTS:
    Q_SCRIPTABLE QString Status() const;
    Q_SCRIPTABLE QString Activity() const;
    Q_SCRIPTABLE void SyncNow();
    // minutes <= 0 pauses until Resume()
    Q_SCRIPTABLE void Pause(int minutes);
    Q_SCRIPTABLE void Resume();
    // Re-read the refresh token after the app signed in, and start syncing
    Q_SCRIPTABLE void ReloadAccount();
    Q_SCRIPTABLE void SignOut();
    // JSON object with name, localPath, driveId, remoteRootId, kind, webUrl, excludedPaths
    Q_SCRIPTABLE QString AddRoot(const QString &json);
    Q_SCRIPTABLE bool RemoveRoot(const QString &id);
    Q_SCRIPTABLE void SetExcludedFolders(const QString &id, const QStringList &paths);
    // action: "delete" or "restore"
    Q_SCRIPTABLE void ResolveBigDelete(const QString &id, const QString &action);
    Q_SCRIPTABLE void Quit();
    // {"command": "pause", "minutes": 120} and the like; returns a JSON result
    Q_SCRIPTABLE QString Command(const QString &json);

Q_SIGNALS:
    Q_SCRIPTABLE void StatusChanged(const QString &status);
    Q_SCRIPTABLE void ActivityAdded(const QString &entry);

private:
    Daemon *m_daemon;
};

}
