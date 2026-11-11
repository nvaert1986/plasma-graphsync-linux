// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pushclient.h"

#include <QTest>
#include <QUrlQuery>

using namespace GraphSync;
using Kind = PushClient::Packet::Kind;

// Messages as captured from the Graph socket.io endpoint in phase 0 (T8)
class PushClientTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void open()
    {
        const auto p = PushClient::parse(
            QStringLiteral("0{\"sid\":\"0TFXL1ylRR_mO_JfGd6j\",\"upgrades\":[],\"pingInterval\":30000,\"pingTimeout\":20000,\"maxPayload\":5000000}"));
        QCOMPARE(p.kind, Kind::Open);
        QCOMPARE(p.pingIntervalMs, 30000);
    }
    void ping()
    {
        QCOMPARE(PushClient::parse(QStringLiteral("2")).kind, Kind::Ping);
    }
    void namespaceConnected()
    {
        const auto p = PushClient::parse(QStringLiteral("40/notifications,{\"sid\":\"QFB6uhKmrZc_YZ0zGd6k\"}"));
        QCOMPARE(p.kind, Kind::NamespaceConnected);
        QCOMPARE(p.nspace, QStringLiteral("/notifications"));
        QCOMPARE(PushClient::parse(QStringLiteral("40")).nspace, QStringLiteral("/"));
    }
    void notification()
    {
        const auto p = PushClient::parse(QStringLiteral("42/notifications,[\"notification\",\"{\\\"value\\\":[{\\\"subscriptionId\\\":\\\"aaaaaaaa\\\",\\\"resource\\\":\\\"bbbbbbbb\\\",\\\"tenantId\\\":\\\"cccccccc\\\"}]}\"]"));
        QCOMPARE(p.kind, Kind::Event);
        QCOMPARE(p.nspace, QStringLiteral("/notifications"));
        QCOMPARE(p.eventName, QStringLiteral("notification"));
    }
    void disconnect()
    {
        QCOMPARE(PushClient::parse(QStringLiteral("41/notifications,")).kind, Kind::NamespaceDisconnected);
    }
    void garbage()
    {
        QCOMPARE(PushClient::parse(QStringLiteral("hello")).kind, Kind::Unknown);
        QCOMPARE(PushClient::parse(QString()).kind, Kind::Unknown);
    }
    void websocketUrl()
    {
        const QUrl url = PushClient::websocketUrl(
            QUrl(QStringLiteral("https://northeurope1-2.pushnp.svc.ms/notifications?token=ns_test&applicationId=dddddddd")));
        QCOMPARE(url.scheme(), QStringLiteral("wss"));
        QCOMPARE(url.host(), QStringLiteral("northeurope1-2.pushnp.svc.ms"));
        QCOMPARE(url.path(), QStringLiteral("/socket.io/"));
        const QUrlQuery query(url);
        QCOMPARE(query.queryItemValue(QStringLiteral("token")), QStringLiteral("ns_test"));
        QCOMPARE(query.queryItemValue(QStringLiteral("EIO")), QStringLiteral("4"));
        QCOMPARE(query.queryItemValue(QStringLiteral("transport")), QStringLiteral("websocket"));
    }
};

QTEST_GUILESS_MAIN(PushClientTest)
#include "pushclienttest.moc"
