// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

namespace GraphSync
{

// Minimal socket.io v4 client for Graph change notifications (phase 0 T8): open the
// websocket, join the namespace from the notification URL, answer pings and report
// "notification" events. Notifications carry no item details; they only mean "run delta".
class PushClient : public QObject
{
    Q_OBJECT
public:
    explicit PushClient(QObject *parent = nullptr);

    // Parsed socket.io packet, exposed for tests
    struct Packet {
        enum class Kind {
            Unknown,
            Open, // engine.io "0{...}"
            Ping, // engine.io "2"
            NamespaceConnected, // socket.io "40/ns,{...}"
            NamespaceDisconnected, // socket.io "41/ns,"
            Event, // socket.io "42/ns,[name, ...]"
        };
        Kind kind = Kind::Unknown;
        QString nspace;
        QString eventName;
        int pingIntervalMs = 0;
    };
    static Packet parse(const QString &message);
    static QUrl websocketUrl(const QUrl &notificationUrl);

    bool isConnected() const;

public Q_SLOTS:
    void setEndpoint(const QString &notificationUrl, const QDateTime &expires);
    void stop();

Q_SIGNALS:
    void notificationReceived();
    // The connection is gone and the endpoint is probably no longer valid
    void endpointLost();

private:
    void connectSocket();
    void onMessage(const QString &message);

    QWebSocket m_socket;
    QUrl m_notificationUrl;
    QString m_namespace;
    QTimer m_reconnect;
    QTimer m_watchdog;
    bool m_joined = false;
    int m_failures = 0;
    QElapsedTimer m_joinedSince;
};

}
