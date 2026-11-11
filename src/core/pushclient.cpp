// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pushclient.h"
#include "graphsync_debug.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

#include <algorithm>

namespace GraphSync
{

PushClient::PushClient(QObject *parent)
    : QObject(parent)
{
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &PushClient::connectSocket);

    // The server pings every pingInterval; silence for much longer means a dead connection
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        qCInfo(GRAPHSYNC) << "Push connection silent, reconnecting";
        m_socket.abort();
    });

    connect(&m_socket, &QWebSocket::textMessageReceived, this, &PushClient::onMessage);
    connect(&m_socket, &QWebSocket::disconnected, this, [this] {
        // Only a connection that lasted counts as healthy: a server that drops it right after
        // the join (seen during a large catch-up) would otherwise get a reconnect every 5 s forever
        if (m_joined && m_joinedSince.isValid() && m_joinedSince.elapsed() > 60 * 1000) {
            m_failures = 0;
        }
        m_joined = false;
        m_watchdog.stop();
        if (m_notificationUrl.isEmpty()) {
            return;
        }
        ++m_failures;
        if (m_failures >= 3) {
            // Repeated failures: the endpoint has probably expired, ask for a new one
            m_failures = 0;
            Q_EMIT endpointLost();
            return;
        }
        m_reconnect.start(std::min(60, 5 * m_failures) * 1000);
    });
}

PushClient::Packet PushClient::parse(const QString &message)
{
    Packet packet;
    if (message == QLatin1String("2")) {
        packet.kind = Packet::Kind::Ping;
        return packet;
    }
    if (message.startsWith(QLatin1Char('0'))) {
        packet.kind = Packet::Kind::Open;
        packet.pingIntervalMs = QJsonDocument::fromJson(message.mid(1).toUtf8()).object().value(QLatin1String("pingInterval")).toInt();
        return packet;
    }
    if (message.size() < 2 || message.at(0) != QLatin1Char('4')) {
        return packet;
    }
    // socket.io packet: type digit, optional "/namespace,", then JSON payload
    QString rest = message.mid(2);
    if (rest.startsWith(QLatin1Char('/'))) {
        const int comma = rest.indexOf(QLatin1Char(','));
        packet.nspace = comma < 0 ? rest : rest.left(comma);
        rest = comma < 0 ? QString() : rest.mid(comma + 1);
    } else {
        packet.nspace = QStringLiteral("/");
    }
    switch (message.at(1).unicode()) {
    case '0':
        packet.kind = Packet::Kind::NamespaceConnected;
        break;
    case '1':
        packet.kind = Packet::Kind::NamespaceDisconnected;
        break;
    case '2': {
        packet.kind = Packet::Kind::Event;
        const QJsonArray args = QJsonDocument::fromJson(rest.toUtf8()).array();
        packet.eventName = args.isEmpty() ? QString() : args.at(0).toString();
        break;
    }
    default:
        break;
    }
    return packet;
}

QUrl PushClient::websocketUrl(const QUrl &notificationUrl)
{
    // socket.io convention: the URL path is the namespace, the transport lives at /socket.io/
    QUrl url;
    url.setScheme(QStringLiteral("wss"));
    url.setHost(notificationUrl.host());
    url.setPort(notificationUrl.port());
    url.setPath(QStringLiteral("/socket.io/"));
    QUrlQuery query(notificationUrl);
    query.addQueryItem(QStringLiteral("EIO"), QStringLiteral("4"));
    query.addQueryItem(QStringLiteral("transport"), QStringLiteral("websocket"));
    url.setQuery(query);
    return url;
}

bool PushClient::isConnected() const
{
    return m_joined;
}

void PushClient::setEndpoint(const QString &notificationUrl, const QDateTime &expires)
{
    Q_UNUSED(expires)
    const QUrl url(notificationUrl);
    if (url == m_notificationUrl && m_socket.state() != QAbstractSocket::UnconnectedState) {
        return;
    }
    m_notificationUrl = url;
    m_namespace = url.path().isEmpty() ? QStringLiteral("/") : url.path();
    m_failures = 0;
    connectSocket();
}

void PushClient::stop()
{
    m_notificationUrl.clear();
    m_reconnect.stop();
    m_watchdog.stop();
    m_socket.close();
}

void PushClient::connectSocket()
{
    if (m_notificationUrl.isEmpty()) {
        return;
    }
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
    }
    m_joined = false;
    m_socket.open(websocketUrl(m_notificationUrl));
}

void PushClient::onMessage(const QString &message)
{
    const Packet packet = parse(message);
    switch (packet.kind) {
    case Packet::Kind::Open:
        m_watchdog.setInterval(std::max(packet.pingIntervalMs, 25000) * 3);
        m_watchdog.start();
        m_socket.sendTextMessage(m_namespace == QLatin1String("/") ? QStringLiteral("40") : QStringLiteral("40%1,").arg(m_namespace));
        break;
    case Packet::Kind::Ping:
        m_socket.sendTextMessage(QStringLiteral("3"));
        m_watchdog.start();
        break;
    case Packet::Kind::NamespaceConnected:
        if (packet.nspace == m_namespace) {
            m_joined = true;
            m_joinedSince.start();
            qCInfo(GRAPHSYNC) << "Push notifications connected";
        }
        break;
    case Packet::Kind::NamespaceDisconnected:
        m_socket.abort();
        break;
    case Packet::Kind::Event:
        m_watchdog.start();
        if (packet.eventName == QLatin1String("notification")) {
            Q_EMIT notificationReceived();
        }
        break;
    case Packet::Kind::Unknown:
        break;
    }
}

}
