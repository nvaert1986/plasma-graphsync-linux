// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

class QLocalServer;
class QLocalSocket;
class QTimer;

namespace GraphSync
{

class Daemon;
class RootController;

// Local socket for Dolphin (and later openVFS), at $XDG_RUNTIME_DIR/plasma-graphsync-linux/socket.
// Newline-delimited messages in the style of the Nextcloud/ownCloud socket API:
//   -> REGISTER_PATH:<root>          sent to every client for each synced folder
//   -> UNREGISTER_PATH:<root>
//   <- RETRIEVE_FILE_STATUS:<path>   (also RETRIEVE_FOLDER_STATUS)
//   -> STATUS:<OK|SYNC|NEW|ERROR|IGNORE|ONLINE|NOP>[+ROOT]:<path>   (ONLINE: Files On-Demand placeholder)
//   -> UPDATE_VIEW:<root>            states under this root changed; ask again
//   <- VERSION:                      -> VERSION:<version>:1.0:<pid>
class SocketServer : public QObject
{
    Q_OBJECT
public:
    explicit SocketServer(Daemon *daemon, QObject *parent = nullptr);
    ~SocketServer() override;

    bool listen();
    static QString socketPath();

    // Status of one absolute path, as sent in STATUS replies (exposed for tests)
    QString statusFor(const QString &path) const;

private:
    void onNewConnection();
    void onReadyRead(QLocalSocket *socket);
    void handleLine(QLocalSocket *socket, const QString &line);
    void send(QLocalSocket *socket, const QString &line);
    void broadcast(const QString &line);
    void watchRoots();
    void scheduleUpdate(const QString &root);

    Daemon *m_daemon;
    QLocalServer *m_server = nullptr;
    QList<QLocalSocket *> m_clients;
    QSet<QString> m_registered;
    QSet<QString> m_pendingUpdates;
    QTimer *m_updateTimer = nullptr;
};

}
