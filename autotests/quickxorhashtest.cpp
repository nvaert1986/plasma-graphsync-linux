// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quickxorhash.h"

#include <QRandomGenerator>
#include <QTemporaryFile>
#include <QTest>

using namespace GraphSync;

class QuickXorHashTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void vectors_data();
    void vectors();
    void chunkingDoesNotMatter();
    void hashFile();
};

// Test vectors from rclone's backend/onedrive/quickxorhash (MIT), which were produced with
// Microsoft's reference implementation: base64 input, base64 expected hash.
void QuickXorHashTest::vectors_data()
{
    QTest::addColumn<QByteArray>("input");
    QTest::addColumn<QString>("expected");
    QTest::newRow("0") << QByteArrayLiteral("") << QStringLiteral("AAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    QTest::newRow("1") << QByteArrayLiteral("Sg==") << QStringLiteral("SgAAAAAAAAAAAAAAAQAAAAAAAAA=");
    QTest::newRow("2") << QByteArrayLiteral("tbQ=") << QStringLiteral("taAFAAAAAAAAAAAAAgAAAAAAAAA=");
    QTest::newRow("3") << QByteArrayLiteral("0pZP") << QStringLiteral("0rDEEwAAAAAAAAAAAwAAAAAAAAA=");
    QTest::newRow("7") << QByteArrayLiteral("qaApEj66lw==") << QStringLiteral("qQBFCiTgA11cAgAABwAAAAAAAAA=");
    QTest::newRow("20") << QByteArrayLiteral("T6LYJIfDh81JrAK309H2JMJTXis=") << QStringLiteral("zBTHrspn3mEcohlJdIUAbjGNaNg=");
    QTest::newRow("63") << QByteArrayLiteral("L+LSB8kmGMnHaWVA5P/+qFnfQliXvgJW7d2JGAgT6+koi5NQujFW1bwQVoXrBVyob/gBxGizUoJMgid5gGNo") << QStringLiteral("ndX/KZBtFoeO3xKeo1ajO/Jy+rY=");
    QTest::newRow("64") << QByteArrayLiteral("Mb7EGva2rEE5fENDL85P+BsapHEEjv2/siVhKjvAQe02feExVOQSkfmuYzU/kTF1MaKjPmKF/w+cbvwfdWL8aQ==") << QStringLiteral("n1anP5NfvD4XDYWIeRPW3ZkPv1Y=");
    QTest::newRow("333") << QByteArrayLiteral("w0LGhqU1WXFbdavqDE4kAjEzWLGGzmTNikzqnsiXHx2KRReKVTxkv27u3UcEz9+lbMvYl4xFf2Z4aE1xRBBNd1Ke5C0zToSaYw5o4B/7X99nKK2/XaUX1byLow2aju2XJl2OpKpJg+tSJ2fmjIJTkfuYUz574dFX6/VXxSxwGH/xQEAKS5TCsBK3CwnuG1p5SAsQq3gGVozDWyjEBcWDMdy8/AIFrj/y03Lfc/RNRCQTAfZbnf2QwV7sluw4fH3XJr07UoD0YqN+7XZzidtrwqMY26fpLZnyZjnBEt1FAZWO7RnKG5asg8xRk9YaDdedXdQSJAOy6bWEWlABj+tVAigBxavaluUH8LOj+yfCFldJjNLdi90fVHkUD/m4Mr5OtmupNMXPwuG3EQlqWUVpQoYpUYKLsk7a5Mvg6UFkiH596y5IbJEVCI1Kb3D1") << QStringLiteral("e3+wo77iKcILiZegnzyUNcjCdoQ=");
}

void QuickXorHashTest::vectors()
{
    QFETCH(QByteArray, input);
    QFETCH(QString, expected);
    QuickXorHash hash;
    hash.addData(QByteArray::fromBase64(input));
    QCOMPARE(hash.base64Result(), expected);
}

void QuickXorHashTest::chunkingDoesNotMatter()
{
    QByteArray data(1000003, Qt::Uninitialized);
    QRandomGenerator rng(42);
    for (char &c : data) {
        c = static_cast<char>(rng.bounded(256));
    }
    QuickXorHash whole;
    whole.addData(data);
    QuickXorHash pieces;
    for (qsizetype offset = 0; offset < data.size(); offset += 997) {
        pieces.addData(data.constData() + offset, std::min<qsizetype>(997, data.size() - offset));
    }
    QCOMPARE(pieces.base64Result(), whole.base64Result());
}

void QuickXorHashTest::hashFile()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    const QByteArray data = QByteArray::fromBase64(QByteArrayLiteral("qaApEj66lw=="));
    file.write(data);
    file.flush();
    QCOMPARE(QuickXorHash::hashFile(file.fileName()), QStringLiteral("qQBFCiTgA11cAgAABwAAAAAAAAA="));
    QVERIFY(QuickXorHash::hashFile(QStringLiteral("/nonexistent/graphsync")).isEmpty());
}

QTEST_GUILESS_MAIN(QuickXorHashTest)
#include "quickxorhashtest.moc"
