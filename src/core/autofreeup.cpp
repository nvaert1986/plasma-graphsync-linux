// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "autofreeup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <algorithm>

namespace GraphSync::AutoFreeUp
{

bool accessTimesKept(const QString &path)
{
    struct statvfs info;
    if (statvfs(QFile::encodeName(path).constData(), &info) != 0) {
        return false;
    }
    return !(info.f_flag & ST_NOATIME);
}

std::optional<qint64> lastUsed(const QString &path, bool useAccessTime)
{
    struct stat info;
    if (lstat(QFile::encodeName(path).constData(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return std::nullopt;
    }
    const qint64 changed = std::max<qint64>(info.st_mtim.tv_sec, info.st_ctim.tv_sec);
    return useAccessTime ? std::max<qint64>(changed, info.st_atim.tv_sec) : changed;
}

bool hasOfficeLock(const QString &path)
{
    const QFileInfo file(path);
    const QString dir = file.absolutePath() + QLatin1Char('/');
    const QString name = file.fileName();
    if (QFileInfo::exists(dir + QStringLiteral(".~lock.") + name + QLatin1Char('#'))) {
        return true;
    }
    // Word and Excel write "~$" in place of the name's first two characters, its first one or none,
    // depending on the length of the name: any of the three counts
    for (qsizetype dropped = 0; dropped <= 2 && dropped < name.size(); ++dropped) {
        if (QFileInfo::exists(dir + QStringLiteral("~$") + name.mid(dropped))) {
            return true;
        }
    }
    return false;
}

QSet<QString> openFiles()
{
    QSet<QString> files;
    // Only the processes of this user can be read; that is everyone who can open these files
    const QStringList pids = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pid : pids) {
        bool numeric = false;
        pid.toInt(&numeric);
        if (!numeric) {
            continue;
        }
        const QString fdDir = QStringLiteral("/proc/") + pid + QStringLiteral("/fd");
        // The descriptors are symlinks: to files (Files), sockets and pipes (System)
        const QStringList fds = QDir(fdDir).entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
        for (const QString &fd : fds) {
            const QString target = QFile::symLinkTarget(fdDir + QLatin1Char('/') + fd);
            if (target.startsWith(QLatin1Char('/'))) {
                files.insert(target);
            }
        }
    }
    return files;
}

}
