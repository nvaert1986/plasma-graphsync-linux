// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "statedb.h"

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
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

    void sharedRoundTrip()
    {
        ItemRow row;
        row.id = QStringLiteral("A");
        row.name = QStringLiteral("a.txt");
        row.shared = true;
        m_db->upsert(row);
        row.id = QStringLiteral("B");
        row.shared = false;
        m_db->upsert(row);
        const auto items = m_db->loadItems();
        QVERIFY(items.value(QStringLiteral("A")).shared);
        QVERIFY(!items.value(QStringLiteral("B")).shared);
    }

    void upgradeAddsSharedColumn()
    {
        // A state database of 0.8 (schema 2, no "shared" column) with one item
        m_db.reset();
        const QString path = QDir(Config::stateDirectory()).filePath(m_rootId + QStringLiteral(".sqlite3"));
        QFile::remove(path);
        {
            QSqlDatabase old = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("old"));
            old.setDatabaseName(path);
            QVERIFY(old.open());
            QSqlQuery q(old);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE items (id TEXT PRIMARY KEY, parent_id TEXT, name TEXT NOT NULL, is_folder INTEGER NOT NULL,"
                                          " r_etag TEXT, r_ctag TEXT, r_hash TEXT, r_size INTEGER, l_hash TEXT, l_size INTEGER, l_mtime_ns INTEGER,"
                                          " l_inode INTEGER, dehydrated INTEGER NOT NULL DEFAULT 0)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO items (id, name, is_folder) VALUES ('X', 'x.txt', 0)")));
            old.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("old"));

        m_db = std::make_unique<StateDb>(m_rootId);
        QString error;
        QVERIFY2(m_db->open(&error), qPrintable(error));
        auto items = m_db->loadItems();
        QCOMPARE(items.size(), 1);
        QVERIFY(!items.value(QStringLiteral("X")).shared);
        ItemRow row = items.value(QStringLiteral("X"));
        row.shared = true;
        m_db->upsert(row);
        QVERIFY(m_db->loadItems().value(QStringLiteral("X")).shared);
        QCOMPARE(m_db->meta(QStringLiteral("schema")), QStringLiteral("4"));
    }

    void openedKeepsTheLatestTime()
    {
        m_db->noteOpened({{QStringLiteral("a.txt"), 2000}, {QStringLiteral("b.txt"), 3000}});
        m_db->noteOpened({{QStringLiteral("a.txt"), 1000}});
        m_db->noteOpened({{QStringLiteral("b.txt"), 4000}});
        auto opened = m_db->loadOpened();
        QCOMPARE(opened.value(QStringLiteral("a.txt")), 2000);
        QCOMPARE(opened.value(QStringLiteral("b.txt")), 4000);
        m_db->removeOpened({QStringLiteral("a.txt")});
        opened = m_db->loadOpened();
        QCOMPARE(opened.size(), 1);
        QVERIFY(opened.contains(QStringLiteral("b.txt")));
    }

    void openedSinceIsWrittenOnce()
    {
        const QString since = m_db->meta(QStringLiteral("openedSince"));
        QVERIFY(since.toLongLong() > 0);
        m_db.reset();
        m_db = std::make_unique<StateDb>(m_rootId);
        QString error;
        QVERIFY(m_db->open(&error));
        QCOMPARE(m_db->meta(QStringLiteral("openedSince")), since);
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
