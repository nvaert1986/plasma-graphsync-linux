// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QSqlDatabase>
#include <QString>

namespace GraphSync
{

// Last synced state of one item. A file has two baselines because SharePoint rewrites Office
// files after upload (property promotion): the remote hash then differs from the local one.
struct ItemRow {
    QString id;
    QString parentId;
    QString name;
    bool isFolder = false;

    // Remote baseline: what the server reported the last time we were in sync
    QString remoteETag;
    QString remoteCTag;
    QString remoteHash;
    qint64 remoteSize = 0;

    // Local baseline: the file on disk at that moment
    QString localHash;
    qint64 localSize = -1;
    qint64 localMtimeNs = 0;
    quint64 localInode = 0;

    // Files On-Demand: only a placeholder (<name>.cloud) is on disk; the local baseline then
    // describes the placeholder file (its inode, to follow renames)
    bool dehydrated = false;

    // Shared with others (its own sharing, from the delta feed); the emblem covers its contents too
    bool shared = false;
};

// Per-root SQLite state. Opened and used from a single thread (the root's sync worker).
class StateDb
{
public:
    explicit StateDb(const QString &rootId);
    ~StateDb();

    bool open(QString *error);

    QString meta(const QString &key) const;
    void setMeta(const QString &key, const QString &value);

    QHash<QString, ItemRow> loadItems() const;
    void upsert(const ItemRow &row);
    void remove(const QString &id);
    void clearItems();

    void begin();
    void commit();

    // Read-only, from another process than the sync engine (the app, graphsyncctl): whether the
    // local file at path is the synced item itemId of location rootId (same inode as recorded;
    // for an online-only file its placeholder's). False when the state does not know the item.
    static bool localMatches(const QString &rootId, const QString &itemId, const QString &path);

    // Files On-Demand pins per relative path: what the user chose for a file or folder
    enum class Pin {
        AlwaysKeep = 1, // downloaded and kept
        OnlineOnly = 2, // only placeholders ("Free up space")
        Available = 3, // opened or downloaded on request: kept until freed up again (overrides an
                       // online-only choice on a folder above, like "available on this device")
    };
    QHash<QString, Pin> loadPins() const;
    // Sets the pin on rel and removes the pins below it (a folder's choice covers its contents)
    void setPin(const QString &rel, Pin pin);
    // A file or folder moved: its pin and the pins below it move along
    void movePins(const QString &fromRel, const QString &toRel);
    void removePins(const QStringList &rels);

    // Automatic free up space: when files were last opened (seconds since the epoch), by
    // relative path, as the file watcher saw it; recorded since meta "openedSince"
    QHash<QString, qint64> loadOpened() const;
    void noteOpened(const QHash<QString, qint64> &opened);
    void removeOpened(const QStringList &rels);
    void clearOpened();

private:
    QString m_connectionName;
    QString m_path;
};

}
