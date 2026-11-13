// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "auth.h"
#include "graphclient.h"

#include <QElapsedTimer>
#include <QTcpServer>
#include <QTest>
#include <QTimer>

#include <atomic>

using namespace GraphSync;

class GraphClientTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // A pause, a stop or the daemon shutting down breaks off a request that is still waiting for
    // an answer, instead of waiting for it (up to the two-minute transfer timeout)
    void cancelBreaksOffARunningRequest()
    {
        // Accepts the connection and never answers: the TLS handshake waits forever
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        Auth auth(AuthConfig{QStringLiteral("0a1b2c3d-4e5f-6789-abcd-ef0123456789"), QStringLiteral("11111111-2222-3333-4444-555555555555"), {}});
        GraphClient graph(&auth);
        std::atomic_bool cancel = false;
        graph.setCancelFlag(&cancel);
        QTimer::singleShot(300, [&cancel] {
            cancel = true;
        });
        QElapsedTimer took;
        took.start();
        const GraphResponse response = graph.get(QStringLiteral("https://127.0.0.1:%1/drives/x/items/y/thumbnails").arg(silent.serverPort()));
        QVERIFY2(took.elapsed() < 3000, qPrintable(QStringLiteral("took %1 ms").arg(took.elapsed())));
        QCOMPARE(response.status, 0);
        // Told apart from being offline by the callers
        QCOMPARE(response.errorString, QStringLiteral("cancelled"));
    }

    // Without a cancel request the same request is not broken off early
    void noCancelKeepsWaiting()
    {
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        Auth auth(AuthConfig{QStringLiteral("0a1b2c3d-4e5f-6789-abcd-ef0123456789"), QStringLiteral("11111111-2222-3333-4444-555555555555"), {}});
        GraphClient graph(&auth);
        std::atomic_bool cancel = false;
        graph.setCancelFlag(&cancel);
        bool stillWaiting = false;
        // Checked from inside the request's wait, then cancelled so the test ends
        QTimer::singleShot(1500, [&] {
            stillWaiting = true;
            cancel = true;
        });
        graph.get(QStringLiteral("https://127.0.0.1:%1/x").arg(silent.serverPort()));
        QVERIFY(stillWaiting);
    }
};

QTEST_GUILESS_MAIN(GraphClientTest)
#include "graphclienttest.moc"
