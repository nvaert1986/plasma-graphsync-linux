// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>

#include <optional>

namespace GraphSync
{

struct LocalEntry {
    bool isFolder = false;
    qint64 size = 0;
    qint64 mtimeNs = 0;
    quint64 inode = 0;
};

namespace LocalScanner
{
// Walks root recursively without following symlinks. Keys are paths relative to root,
// '/'-separated, without a leading slash. Ignored names (and everything below an ignored
// folder) are left out.
QHash<QString, LocalEntry> scan(const QString &root);
// Same, but the folders in skip (relative paths) are listed without their contents
QHash<QString, LocalEntry> scan(const QString &root, const QSet<QString> &skip);

std::optional<LocalEntry> stat(const QString &absolutePath);
// Sets the modification time (the access time stays); false when time is invalid or it failed
bool setModified(const QString &absolutePath, const QDateTime &time);
}

}
