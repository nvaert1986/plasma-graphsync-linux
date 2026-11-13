// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QSet>
#include <QString>

#include <optional>

namespace GraphSync::AutoFreeUp
{

// Automatic free up space (Files On-Demand): files not opened for a while become online-only
// again, like Storage Sense on Windows. Use is the latest of: the change time, the inode change
// time (a download, rename or change; kept also with noatime), the opens the file watcher saw,
// and the access time where the file system keeps it (relatime, to within a day).

// False on a file system mounted with noatime: there the access time says nothing
bool accessTimesKept(const QString &path);

// When the file was last changed, downloaded, renamed or (with useAccessTime) read, in seconds
// since the epoch; nothing when it is no regular file
std::optional<qint64> lastUsed(const QString &path, bool useAccessTime);

// An office suite has the file open: LibreOffice's .~lock.<name># or Microsoft Office's ~$ owner file
bool hasOfficeLock(const QString &path);

// Files that a process of this user has open right now (absolute paths, from /proc/<pid>/fd)
QSet<QString> openFiles();

}
