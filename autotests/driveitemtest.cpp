// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "driveitem.h"
#include "ignorerules.h"

#include <QJsonDocument>
#include <QTest>

using namespace GraphSync;

class DriveItemTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void safeNames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("safe");
        QTest::newRow("plain") << QStringLiteral("Report.docx") << true;
        QTest::newRow("spaces and unicode") << QStringLiteral("Plan ✓ 2026 (final).xlsx") << true;
        QTest::newRow("dots inside") << QStringLiteral("a..b") << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("dot") << QStringLiteral(".") << false;
        QTest::newRow("dot dot") << QStringLiteral("..") << false;
        QTest::newRow("slash") << QStringLiteral("../etc") << false;
        QTest::newRow("backslash") << QStringLiteral("a\\b") << false;
        QTest::newRow("NUL") << QStringLiteral("a") + QChar(0) + QStringLiteral("b") << false;
        QTest::newRow("256 bytes") << QString(256, QLatin1Char('x')) << false;
        QTest::newRow("255 bytes") << QString(255, QLatin1Char('x')) << true;
    }
    void safeNames()
    {
        QFETCH(QString, name);
        QFETCH(bool, safe);
        QCOMPARE(DriveItem::isSafeName(name), safe);
    }

    void safeIds()
    {
        QVERIFY(DriveItem::isSafeId(QStringLiteral("01ABCDEFGHIJKLMNOPQRSTUVWXYZ234567")));
        QVERIFY(DriveItem::isSafeId(QStringLiteral("b!AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")));
        QVERIFY(DriveItem::isSafeId(QStringLiteral("ABC123!456")));
        QVERIFY(!DriveItem::isSafeId(QString()));
        QVERIFY(!DriveItem::isSafeId(QStringLiteral("../x")));
        QVERIFY(!DriveItem::isSafeId(QStringLiteral("a b")));
    }

    void sharedFacet()
    {
        // Delta asked with "Prefer: hierarchicalsharing": only items with sharing of their own
        // carry the facet; the root carries an empty one, which is not "shared with others"
        const auto parse = [](const char *json) {
            return DriveItem::fromJson(QJsonDocument::fromJson(json).object());
        };
        QVERIFY(parse(R"({"id":"A","name":"a.txt","file":{},"shared":{"scope":"users"}})").shared);
        QVERIFY(parse(R"({"id":"B","name":"b","folder":{},"shared":{"scope":"organization"}})").shared);
        QVERIFY(!parse(R"({"id":"C","name":"c.txt","file":{}})").shared);
        QVERIFY(!parse(R"({"id":"R","name":"root","folder":{},"root":{},"shared":{}})").shared);
    }

    void ignoredNamesIncludeOldTempFiles()
    {
        QVERIFY(IgnoreRules::isIgnored(QStringLiteral(".graphsync-tmp-01ABC")));
        QVERIFY(IgnoreRules::isIgnored(QStringLiteral(".podl-tmp-01ABC")));
    }
};

QTEST_GUILESS_MAIN(DriveItemTest)

#include "driveitemtest.moc"
