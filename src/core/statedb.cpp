// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "statedb.h"
#include "config.h"
#include "graphsync_debug.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

#include <sys/stat.h>

namespace GraphSync
{

namespace
{
// 2: items.dehydrated and the pins table (Files On-Demand); 3: items.shared; 4: the opened table
constexpr int schemaVersion = 4;

void exec(QSqlQuery &query)
{
    if (!query.exec()) {
        qCWarning(GRAPHSYNC) << "SQL error:" << query.lastError().text() << query.lastQuery();
    }
}
}

StateDb::StateDb(const QString &rootId)
    : m_connectionName(QStringLiteral("graphsync-%1-%2").arg(rootId).arg(reinterpret_cast<quintptr>(QThread::currentThread()), 0, 16))
    , m_path(QDir(Config::stateDirectory()).filePath(rootId + QStringLiteral(".sqlite3")))
{
}

StateDb::~StateDb()
{
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isOpen()) {
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool StateDb::open(QString *error)
{
    // A log without its database is a leftover of an earlier location with this id: never replayed
    if (!QFile::exists(m_path)) {
        QFile::remove(m_path + QStringLiteral("-wal"));
        QFile::remove(m_path + QStringLiteral("-shm"));
    }
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(m_path);
    if (!db.open()) {
        *error = db.lastError().text();
        return false;
    }
    // Owner only; SQLite gives the -wal and -shm files the same rights as the database
    for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        QFile::setPermissions(m_path + suffix, QFile::ReadOwner | QFile::WriteOwner);
    }
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS items ("
        " id TEXT PRIMARY KEY, parent_id TEXT, name TEXT NOT NULL, is_folder INTEGER NOT NULL,"
        " r_etag TEXT, r_ctag TEXT, r_hash TEXT, r_size INTEGER,"
        " l_hash TEXT, l_size INTEGER, l_mtime_ns INTEGER, l_inode INTEGER)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS items_parent ON items(parent_id)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS pins (rel TEXT PRIMARY KEY, state INTEGER NOT NULL)"));
    // Upgrade from schema 1; a column that already exists makes this fail harmlessly
    q.exec(QStringLiteral("ALTER TABLE items ADD COLUMN dehydrated INTEGER NOT NULL DEFAULT 0"));
    q.exec(QStringLiteral("ALTER TABLE items ADD COLUMN shared INTEGER NOT NULL DEFAULT 0"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS opened (rel TEXT PRIMARY KEY, at INTEGER NOT NULL)"));
    // Opens are known from here on; before it, a file counts as used now
    if (meta(QStringLiteral("openedSince")).isEmpty()) {
        setMeta(QStringLiteral("openedSince"), QString::number(QDateTime::currentSecsSinceEpoch()));
    }
    if (meta(QStringLiteral("schema")).toInt() < schemaVersion) {
        setMeta(QStringLiteral("schema"), QString::number(schemaVersion));
    }
    return true;
}

QString StateDb::meta(const QString &key) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    q.addBindValue(key);
    exec(q);
    return q.next() ? q.value(0).toString() : QString();
}

void StateDb::setMeta(const QString &key, const QString &value)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral("INSERT INTO meta (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    exec(q);
}

QHash<QString, ItemRow> StateDb::loadItems() const
{
    QHash<QString, ItemRow> items;
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.setForwardOnly(true);
    q.prepare(QStringLiteral(
        "SELECT id, parent_id, name, is_folder, r_etag, r_ctag, r_hash, r_size, l_hash, l_size, l_mtime_ns, l_inode, dehydrated, shared FROM items"));
    exec(q);
    while (q.next()) {
        ItemRow row;
        row.id = q.value(0).toString();
        row.parentId = q.value(1).toString();
        row.name = q.value(2).toString();
        row.isFolder = q.value(3).toBool();
        row.remoteETag = q.value(4).toString();
        row.remoteCTag = q.value(5).toString();
        row.remoteHash = q.value(6).toString();
        row.remoteSize = q.value(7).toLongLong();
        row.localHash = q.value(8).toString();
        row.localSize = q.value(9).toLongLong();
        row.localMtimeNs = q.value(10).toLongLong();
        row.localInode = q.value(11).toULongLong();
        row.dehydrated = q.value(12).toBool();
        row.shared = q.value(13).toBool();
        items.insert(row.id, row);
    }
    return items;
}

void StateDb::upsert(const ItemRow &row)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO items (id, parent_id, name, is_folder, r_etag, r_ctag, r_hash, r_size, l_hash, l_size, l_mtime_ns, l_inode, dehydrated, shared)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(row.id);
    q.addBindValue(row.parentId);
    q.addBindValue(row.name);
    q.addBindValue(row.isFolder);
    q.addBindValue(row.remoteETag);
    q.addBindValue(row.remoteCTag);
    q.addBindValue(row.remoteHash);
    q.addBindValue(row.remoteSize);
    q.addBindValue(row.localHash);
    q.addBindValue(row.localSize);
    q.addBindValue(row.localMtimeNs);
    q.addBindValue(static_cast<qint64>(row.localInode));
    q.addBindValue(row.dehydrated);
    q.addBindValue(row.shared);
    exec(q);
}

QHash<QString, StateDb::Pin> StateDb::loadPins() const
{
    QHash<QString, Pin> pins;
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT rel, state FROM pins"));
    exec(q);
    while (q.next()) {
        const int state = q.value(1).toInt();
        if (state == int(Pin::AlwaysKeep) || state == int(Pin::OnlineOnly) || state == int(Pin::Available)) {
            pins.insert(q.value(0).toString(), Pin(state));
        }
    }
    return pins;
}

void StateDb::setPin(const QString &rel, Pin pin)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    // rel "" is the whole root: every pin goes
    q.prepare(rel.isEmpty() ? QStringLiteral("DELETE FROM pins") : QStringLiteral("DELETE FROM pins WHERE rel = ? OR rel LIKE ? ESCAPE '\\'"));
    if (!rel.isEmpty()) {
        QString like = rel;
        like.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('%'), QStringLiteral("\\%")).replace(QLatin1Char('_'), QStringLiteral("\\_"));
        q.addBindValue(rel);
        q.addBindValue(QString(like + QStringLiteral("/%")));
    }
    exec(q);
    q.prepare(QStringLiteral("INSERT INTO pins (rel, state) VALUES (?, ?)"));
    q.addBindValue(rel);
    q.addBindValue(int(pin));
    exec(q);
}

void StateDb::movePins(const QString &fromRel, const QString &toRel)
{
    if (fromRel.isEmpty() || fromRel == toRel) {
        return;
    }
    const auto below = [](const QString &rel, const QString &base) {
        return rel == base || rel.startsWith(base + QLatin1Char('/'));
    };
    const QHash<QString, Pin> pins = loadPins();
    QList<QPair<QString, Pin>> moved;
    QStringList gone;
    for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) {
        if (below(it.key(), fromRel)) {
            moved.append({toRel + it.key().mid(fromRel.size()), it.value()});
            gone.append(it.key());
        } else if (below(it.key(), toRel)) {
            // A pin already at the destination gives way to the moved one
            gone.append(it.key());
        }
    }
    if (moved.isEmpty()) {
        return;
    }
    removePins(gone);
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO pins (rel, state) VALUES (?, ?)"));
    for (const auto &[rel, pin] : std::as_const(moved)) {
        q.addBindValue(rel);
        q.addBindValue(int(pin));
        exec(q);
    }
}

void StateDb::removePins(const QStringList &rels)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral("DELETE FROM pins WHERE rel = ?"));
    for (const QString &rel : rels) {
        q.addBindValue(rel);
        exec(q);
    }
}

QHash<QString, qint64> StateDb::loadOpened() const
{
    QHash<QString, qint64> opened;
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.setForwardOnly(true);
    q.prepare(QStringLiteral("SELECT rel, at FROM opened"));
    exec(q);
    while (q.next()) {
        opened.insert(q.value(0).toString(), q.value(1).toLongLong());
    }
    return opened;
}

void StateDb::noteOpened(const QHash<QString, qint64> &opened)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral("INSERT INTO opened (rel, at) VALUES (?, ?) ON CONFLICT(rel) DO UPDATE SET at = max(at, excluded.at)"));
    for (auto it = opened.constBegin(); it != opened.constEnd(); ++it) {
        q.addBindValue(it.key());
        q.addBindValue(it.value());
        exec(q);
    }
}

void StateDb::removeOpened(const QStringList &rels)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral("DELETE FROM opened WHERE rel = ?"));
    for (const QString &rel : rels) {
        q.addBindValue(rel);
        exec(q);
    }
}

void StateDb::clearOpened()
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.exec(QStringLiteral("DELETE FROM opened"));
}

void StateDb::remove(const QString &id)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral("DELETE FROM items WHERE id = ?"));
    q.addBindValue(id);
    exec(q);
}

void StateDb::clearItems()
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.exec(QStringLiteral("DELETE FROM items"));
}

bool StateDb::localMatches(const QString &rootId, const QString &itemId, const QString &path)
{
    const QString file = QDir(Config::stateDirectory()).filePath(rootId + QStringLiteral(".sqlite3"));
    if (!QFile::exists(file)) {
        return false;
    }
    const QString name = QStringLiteral("graphsync-check-%1").arg(reinterpret_cast<quintptr>(QThread::currentThread()), 0, 16);
    bool matches = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(file);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"));
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT l_inode, dehydrated FROM items WHERE id = ?"));
            q.addBindValue(itemId);
            if (q.exec() && q.next()) {
                const quint64 inode = q.value(0).toULongLong();
                const bool dehydrated = q.value(1).toBool();
                struct stat info;
                const QByteArray local = QFile::encodeName(dehydrated ? path + QStringLiteral(".cloud") : path);
                matches = inode != 0 && ::lstat(local.constData(), &info) == 0 && quint64(info.st_ino) == inode;
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return matches;
}

void StateDb::begin()
{
    QSqlDatabase::database(m_connectionName).transaction();
}

void StateDb::commit()
{
    QSqlDatabase::database(m_connectionName).commit();
}

}
