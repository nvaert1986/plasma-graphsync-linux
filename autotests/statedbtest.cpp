// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "statedb.h"

#include <QDir>
#include <QStandardPaths>
#include <QTest>
#include <QUuid>

using namespace GraphSync;

using Pin = StateDb::Pin;

class StateDbTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        m_rootId = QUuid::createUuid().toString(QUuid::Id128).left(8);
        m_db = std::make_unique<StateDb>(m_rootId);
        QString error;
        QVERIFY2(m_db->open(&error), qPrintable(error));
    }

    void cleanup()
    {
        m_db.reset();
        QFile::remove(QDir(Config::stateDirectory()).filePath(m_rootId + QStringLiteral(".sqlite3")));
    }

    void setPinReplacesPinsBelow()
    {
        m_db->setPin(QStringLiteral("a/b/file.txt"), Pin::OnlineOnly);
        m_db->setPin(QStringLiteral("ab/file.txt"), Pin::OnlineOnly);
        m_db->setPin(QStringLiteral("a"), Pin::AlwaysKeep);
        const auto pins = m_db->loadPins();
        QCOMPARE(pins.size(), 2);
        QCOMPARE(pins.value(QStringLiteral("a")), Pin::AlwaysKeep);
        // "ab" only shares a prefix with "a"; it is not below it
        QCOMPARE(pins.value(QStringLiteral("ab/file.txt")), Pin::OnlineOnly);
    }

    void movePinsMovesTheSubtreeOnly()
    {
        m_db->setPin(QStringLiteral("a"), Pin::AlwaysKeep);
        m_db->setPin(QStringLiteral("a/sub/x.docx"), Pin::OnlineOnly);
        m_db->setPin(QStringLiteral("ab/y.txt"), Pin::OnlineOnly);
        m_db->movePins(QStringLiteral("a"), QStringLiteral("renamed"));
        const auto pins = m_db->loadPins();
        QCOMPARE(pins.size(), 3);
        QCOMPARE(pins.value(QStringLiteral("renamed")), Pin::AlwaysKeep);
        QCOMPARE(pins.value(QStringLiteral("renamed/sub/x.docx")), Pin::OnlineOnly);
        QCOMPARE(pins.value(QStringLiteral("ab/y.txt")), Pin::OnlineOnly);
        QVERIFY(!pins.contains(QStringLiteral("a")));
    }

    void movePinsDestinationGivesWay()
    {
        m_db->setPin(QStringLiteral("old"), Pin::OnlineOnly);
        m_db->setPin(QStringLiteral("new"), Pin::AlwaysKeep);
        m_db->setPin(QStringLiteral("new/inner.txt"), Pin::OnlineOnly);
        m_db->movePins(QStringLiteral("old"), QStringLiteral("new"));
        const auto pins = m_db->loadPins();
        QCOMPARE(pins.size(), 1);
        QCOMPARE(pins.value(QStringLiteral("new")), Pin::OnlineOnly);
    }

    void availableOverridesAndSurvives()
    {
        // A file opened in a freed-up folder keeps its own "available" pin; freeing the folder
        // again (setPin on the folder) removes it
        m_db->setPin(QStringLiteral("docs"), Pin::OnlineOnly);
        m_db->setPin(QStringLiteral("docs/report.docx"), Pin::Available);
        auto pins = m_db->loadPins();
        QCOMPARE(pins.value(QStringLiteral("docs/report.docx")), Pin::Available);
        QCOMPARE(pins.value(QStringLiteral("docs")), Pin::OnlineOnly);
        m_db->setPin(QStringLiteral("docs"), Pin::OnlineOnly);
        pins = m_db->loadPins();
        QVERIFY(!pins.contains(QStringLiteral("docs/report.docx")));
    }

    void movePinsWithoutPinsChangesNothing()
    {
        m_db->setPin(QStringLiteral("keep"), Pin::AlwaysKeep);
        m_db->movePins(QStringLiteral("other"), QStringLiteral("keep"));
        QCOMPARE(m_db->loadPins().value(QStringLiteral("keep")), Pin::AlwaysKeep);
    }

    void movePinsNonBmpNames()
    {
        // Emoji are two UTF-16 units: the path rewrite must not cut them
        m_db->setPin(QStringLiteral("map 😀/doc.txt"), Pin::OnlineOnly);
        m_db->movePins(QStringLiteral("map 😀"), QStringLiteral("😀 map 😀"));
        const auto pins = m_db->loadPins();
        QCOMPARE(pins.size(), 1);
        QCOMPARE(pins.value(QStringLiteral("😀 map 😀/doc.txt")), Pin::OnlineOnly);
    }

    void removePinsExactOnly()
    {
        m_db->setPin(QStringLiteral("a"), Pin::AlwaysKeep);
        m_db->setPin(QStringLiteral("a/b.txt"), Pin::OnlineOnly);
        m_db->removePins({QStringLiteral("a")});
        const auto pins = m_db->loadPins();
        QCOMPARE(pins.size(), 1);
        QVERIFY(pins.contains(QStringLiteral("a/b.txt")));
    }

private:
    QString m_rootId;
    std::unique_ptr<StateDb> m_db;
};

QTEST_GUILESS_MAIN(StateDbTest)

#include "statedbtest.moc"
