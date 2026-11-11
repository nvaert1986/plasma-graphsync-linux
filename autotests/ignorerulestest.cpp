// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ignorerules.h"

#include <QTest>

using namespace GraphSync;

class IgnoreRulesTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void ignored_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("ignored");
        QTest::newRow("libreoffice lock") << QStringLiteral(".~lock.Report.docx#") << true;
        QTest::newRow("office owner file") << QStringLiteral("~$Report.docx") << true;
        QTest::newRow("our temp file") << QStringLiteral(".graphsync-tmp-01ABC") << true;
        QTest::newRow("dolphin metadata") << QStringLiteral(".directory") << true;
        QTest::newRow("trash folder") << QStringLiteral(".Trash-1000") << true;
        QTest::newRow("partial download") << QStringLiteral("big.iso.part") << true;
        QTest::newRow("backup tilde") << QStringLiteral("notes.txt~") << true;
        QTest::newRow("normal docx") << QStringLiteral("Report.docx") << false;
        QTest::newRow("hidden but normal") << QStringLiteral(".config.json") << false;
        QTest::newRow("dotted name") << QStringLiteral("v1.2.release notes.txt") << false;
    }
    void ignored()
    {
        QFETCH(QString, name);
        QFETCH(bool, ignored);
        QCOMPARE(IgnoreRules::isIgnored(name), ignored);
    }

    void remoteNames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("valid");
        QTest::newRow("plain") << QStringLiteral("Quarterly report.xlsx") << true;
        QTest::newRow("unicode") << QStringLiteral("Overzicht €uro – ü.docx") << true;
        QTest::newRow("colon") << QStringLiteral("10:30 meeting.txt") << false;
        QTest::newRow("question mark") << QStringLiteral("why?.txt") << false;
        QTest::newRow("pipe") << QStringLiteral("a|b") << false;
        QTest::newRow("trailing space") << QStringLiteral("name ") << false;
        QTest::newRow("trailing dot") << QStringLiteral("name.") << false;
        QTest::newRow("reserved CON") << QStringLiteral("CON.txt") << false;
        QTest::newRow("reserved COM1") << QStringLiteral("com1") << false;
        QTest::newRow("COMPANY is fine") << QStringLiteral("COMPANY.txt") << true;
        QTest::newRow("vti") << QStringLiteral("_vti_cnf") << false;
    }
    void remoteNames()
    {
        QFETCH(QString, name);
        QFETCH(bool, valid);
        QCOMPARE(IgnoreRules::invalidRemoteNameReason(name).isEmpty(), valid);
    }
};

QTEST_GUILESS_MAIN(IgnoreRulesTest)
#include "ignorerulestest.moc"
