// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// The token lock of Auth never waits on itself and never makes a D-Bus handler or the UI wait for
// a sync thread: calls re-entered from its local event loop (a token refresh waiting for the
// network) return at once, and what they asked for (new settings, a sign-out) is done by the
// holder as it lets go.
//
// Token requests go to a slow loopback server of the test, never to Microsoft. KWallet must not be
// the real one (a sign-out deletes entries): run it with .dev/test-auth-lock.sh, on a session bus
// without KWallet.

#include "auth.h"
#include "config.h"

#include <QElapsedTimer>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <atomic>

using namespace GraphSync;

namespace
{
const QString clientA = QStringLiteral("0a1b2c3d-4e5f-6789-abcd-ef0123456789");
const QString clientB = QStringLiteral("99999999-8888-7777-6666-555555555555");
const QString tenant = QStringLiteral("11111111-2222-3333-4444-555555555555");
const QString userA = QStringLiteral("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
const QString userB = QStringLiteral("bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee");

// Answers every token request after delayMs with "invalid_grant"
class SlowLoginServer : public QTcpServer
{
public:
    explicit SlowLoginServer(int delayMs)
        : m_delayMs(delayMs)
    {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    socket->readAll();
                    if (socket->property("answered").toBool()) {
                        return;
                    }
                    socket->setProperty("answered", true);
                    ++requests;
                    QTimer::singleShot(m_delayMs, socket, [socket] {
                        const QByteArray body = R"({"error":"invalid_grant","error_description":"test server"})";
                        socket->write("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                      + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort()));
    }
    std::atomic_int requests = 0;

private:
    int m_delayMs;
};
}

class AuthLockTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        if (qEnvironmentVariableIsEmpty("GRAPHSYNC_TEST_PRIVATE_BUS")) {
            QSKIP("needs a session bus without KWallet: run .dev/test-auth-lock.sh");
        }
        QStandardPaths::setTestModeEnabled(true);
        GraphCore::Accounts::setApplication(QStringLiteral("plasma-graphsync-linux"));
    }

    void testServerOnlyOnLoopback()
    {
        Auth::setLoginServerForTesting(QUrl(QStringLiteral("http://example.com:8080")));
        Auth auth({clientA, tenant, userA});
        auth.setRefreshTokenForTesting(QStringLiteral("t"));
        SlowLoginServer server(0);
        Auth::setLoginServerForTesting(server.url());
        QString error;
        auth.accessToken(&error, true);
        QCOMPARE(server.requests.load(), 1);
        Auth::setLoginServerForTesting({});
    }

    void unreachableWalletIsNotSignedOut()
    {
        // No KWallet on this bus: the read fails, which is not the same as "no sign-in stored"
        Auth auth({clientA, tenant, userA});
        QVERIFY(!auth.hasStoredRefreshToken());
        QVERIFY(auth.keychainUnavailable());
    }

    void reentrantCallsDoNotWait()
    {
        // The current account, so a sign-out that the holder carries out shows in the settings
        GraphCore::Accounts::setDefaultClientId(clientA);
        AccountInfo account;
        account.tenantId = tenant;
        account.objectId = userA;
        account.clientId = clientA;
        Config::registerAccount(account);
        QVERIFY(Config::account().isValid());

        SlowLoginServer server(400);
        Auth::setLoginServerForTesting(server.url());
        Auth auth({clientA, tenant, userA});
        auth.setRefreshTokenForTesting(QStringLiteral("refresh-token-of-a"));

        bool outerRunning = true;
        bool ranNested = false;
        bool nestedHasToken = false;
        QString nestedError;
        qint64 slowestMs = 0;
        QTimer::singleShot(100, this, [&] {
            if (!outerRunning) {
                return;
            }
            ranNested = true;
            QElapsedTimer timer;
            timer.start();
            nestedHasToken = auth.hasStoredRefreshToken(); // would deadlock on a plain mutex
            auth.accessToken(&nestedError);
            auth.setConfig({clientB, tenant, userB});
            auth.logout();
            slowestMs = timer.elapsed();
        });
        // Holds the token lock across the refresh, which waits for the server in a local event loop
        QString error;
        auth.accessToken(&error, true);
        outerRunning = false;
        Auth::setLoginServerForTesting({});

        QVERIFY2(ranNested, "the timer did not fire inside the token refresh");
        QCOMPARE(server.requests.load(), 1);
        QVERIFY2(slowestMs < 100, qPrintable(QStringLiteral("re-entered calls took %1 ms").arg(slowestMs)));
        QVERIFY(nestedHasToken);
        QVERIFY(nestedError.contains(QLatin1String("busy")));
        QVERIFY(error.contains(QLatin1String("sign-in expired")));
        // Done by the holder as it let go: first the sign-out (of account A), then the new settings
        QVERIFY(!Config::account().isValid());
        QCOMPARE(auth.config().clientId, clientB);
        QCOMPARE(auth.config().objectId, userB);
    }

    void otherThreadsDoNotWait()
    {
        SlowLoginServer server(20);
        Auth::setLoginServerForTesting(server.url());
        Auth auth({clientA, tenant, userA});
        std::atomic_bool stop = false;
        std::atomic_int holds = 0;
        // A sync thread: holds the token lock over and over, across token refreshes on the
        // network and KWallet reads
        QThread *worker = QThread::create([&] {
            while (!stop.load()) {
                auth.setRefreshTokenForTesting(QStringLiteral("t"));
                QString error;
                auth.accessToken(&error, true);
                auth.hasStoredRefreshToken();
                ++holds;
            }
        });
        worker->start();
        QElapsedTimer startup;
        startup.start();
        while (holds.load() < 3 && !startup.hasExpired(10000)) {
            QCoreApplication::processEvents(); // the server lives on this thread
        }

        // The main thread (D-Bus handlers, the UI) meanwhile: none of these may wait for it
        qint64 slowestMs = 0;
        QElapsedTimer total;
        total.start();
        int rounds = 0;
        // Three seconds of the main thread and the sync thread at it at the same time
        for (int i = 0; !total.hasExpired(3000); ++i, ++rounds) {
            QElapsedTimer timer;
            timer.start();
            auth.hasStoredRefreshToken();
            auth.setConfig({i % 2 ? clientA : clientB, tenant, i % 2 ? userA : userB});
            auth.config();
            if (i % 10 == 0) {
                auth.logout();
            }
            slowestMs = std::max(slowestMs, timer.elapsed());
            QCoreApplication::processEvents();
        }
        const qint64 totalMs = total.elapsed();
        stop = true;
        QElapsedTimer ending;
        ending.start();
        while (!worker->isFinished() && !ending.hasExpired(10000)) {
            QCoreApplication::processEvents();
        }
        const bool finished = worker->wait(100);
        Auth::setLoginServerForTesting({});
        QVERIFY2(finished, "the sync thread did not finish: deadlock");
        delete worker;

        QVERIFY2(holds.load() > 3, "the sync thread did not hold the lock meanwhile");
        QVERIFY2(server.requests.load() > 3, "no token refreshes reached the server");
        QVERIFY2(slowestMs < 200, qPrintable(QStringLiteral("a call on the main thread waited %1 ms").arg(slowestMs)));
        qInfo().noquote() << QStringLiteral("%1 rounds in %2 ms, slowest call %3 ms; the sync thread held the lock %4 times, %5 refreshes")
                                 .arg(rounds)
                                 .arg(totalMs)
                                 .arg(slowestMs)
                                 .arg(holds.load())
                                 .arg(server.requests.load());
    }
};

QTEST_GUILESS_MAIN(AuthLockTest)
#include "authlocktest.moc"
