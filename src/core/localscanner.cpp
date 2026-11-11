// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "localscanner.h"
#include "ignorerules.h"

#include <QDir>
#include <QFile>

#include <sys/stat.h>

namespace GraphSync::LocalScanner
{

std::optional<LocalEntry> stat(const QString &absolutePath)
{
    struct stat st {
    };
    if (::lstat(QFile::encodeName(absolutePath).constData(), &st) != 0) {
        return std::nullopt;
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        // Symlinks, sockets, fifos and devices are not synced
        return std::nullopt;
    }
    LocalEntry entry;
    entry.isFolder = S_ISDIR(st.st_mode);
    entry.size = entry.isFolder ? 0 : static_cast<qint64>(st.st_size);
    entry.mtimeNs = static_cast<qint64>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    entry.inode = static_cast<quint64>(st.st_ino);
    return entry;
}

namespace
{
void scanDir(const QString &root, const QString &relDir, QHash<QString, LocalEntry> &out, const QSet<QString> &skip)
{
    const QDir dir(relDir.isEmpty() ? root : root + QLatin1Char('/') + relDir);
    const QStringList names = dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        if (IgnoreRules::isIgnored(name)) {
            continue;
        }
        const QString rel = relDir.isEmpty() ? name : relDir + QLatin1Char('/') + name;
        const auto entry = stat(root + QLatin1Char('/') + rel);
        if (!entry) {
            continue;
        }
        out.insert(rel, *entry);
        if (entry->isFolder && !skip.contains(rel)) {
            scanDir(root, rel, out, skip);
        }
    }
}
}

QHash<QString, LocalEntry> scan(const QString &root)
{
    return scan(root, {});
}

QHash<QString, LocalEntry> scan(const QString &root, const QSet<QString> &skip)
{
    QHash<QString, LocalEntry> out;
    scanDir(root, QString(), out, skip);
    return out;
}

}
