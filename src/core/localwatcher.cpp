// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "localwatcher.h"
#include "ignorerules.h"
#include "graphsync_debug.h"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QSocketNotifier>

#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace GraphSync
{

namespace
{
constexpr uint32_t watchMask = IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF
    | IN_ATTRIB | IN_ONLYDIR | IN_DONT_FOLLOW;
// Only with automatic free up space on: it records use and never starts a sync, so the engine's
// own reads cannot loop; off, programs opening thousands of files do not wake us at all
}

LocalWatcher::LocalWatcher(const QString &root, QObject *parent)
    : QObject(parent)
    , m_root(root)
{
    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_fd < 0) {
        qCWarning(GRAPHSYNC) << "inotify_init1 failed:" << strerror(errno) << "- relying on periodic scans";
        return;
    }
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &LocalWatcher::readEvents);
    addRecursive(m_root);
}

LocalWatcher::~LocalWatcher()
{
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

bool LocalWatcher::isActive() const
{
    return m_fd >= 0 && !m_watches.isEmpty();
}

void LocalWatcher::addRecursive(const QString &path)
{
    const int wd = inotify_add_watch(m_fd, QFile::encodeName(path).constData(), mask());
    if (wd < 0) {
        if (errno == ENOSPC) {
            qCWarning(GRAPHSYNC) << "inotify watch limit reached (fs.inotify.max_user_watches); changes below" << path
                            << "are picked up by the periodic scan";
        }
        return;
    }
    m_watches.insert(wd, path);
    const QStringList subdirs = QDir(path).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QString &name : subdirs) {
        if (!IgnoreRules::isIgnored(name)) {
            addRecursive(path + QLatin1Char('/') + name);
        }
    }
}

uint32_t LocalWatcher::mask() const
{
    return m_trackOpens ? (watchMask | IN_OPEN) : watchMask;
}

void LocalWatcher::setTrackOpens(bool on)
{
    if (on == m_trackOpens || m_fd < 0) {
        return;
    }
    m_trackOpens = on;
    // Adding a watch for a path watched already replaces its mask
    for (auto it = m_watches.cbegin(); it != m_watches.cend(); ++it) {
        inotify_add_watch(m_fd, QFile::encodeName(it.value()).constData(), mask());
    }
}

void LocalWatcher::readEvents()
{
    alignas(struct inotify_event) char buffer[16384];
    bool relevant = false;
    QSet<QString> opened;
    while (true) {
        const ssize_t len = ::read(m_fd, buffer, sizeof(buffer));
        if (len <= 0) {
            break;
        }
        for (char *ptr = buffer; ptr < buffer + len;) {
            const auto *event = reinterpret_cast<const struct inotify_event *>(ptr);
            ptr += sizeof(struct inotify_event) + event->len;

            const QString dir = m_watches.value(event->wd);
            if (event->mask & IN_IGNORED) {
                m_watches.remove(event->wd);
                continue;
            }
            if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                if (dir == m_root) {
                    qCWarning(GRAPHSYNC) << "Sync root" << m_root << "was removed or moved";
                    Q_EMIT rootLost();
                }
                continue;
            }
            if (event->mask & IN_Q_OVERFLOW) {
                relevant = true;
                continue;
            }
            const QString name = event->len ? QFile::decodeName(event->name) : QString();
            if (name.isEmpty() || IgnoreRules::isIgnored(name)) {
                continue;
            }
            if (event->mask & IN_OPEN) {
                // "#<inode>" is how inotify names an unnamed temporary file (O_TMPFILE, QSaveFile)
                const bool unnamed = name.startsWith(QLatin1Char('#')) && std::all_of(name.cbegin() + 1, name.cend(), [](QChar c) {
                                         return c.isDigit();
                                     });
                if (!(event->mask & IN_ISDIR) && !unnamed) {
                    opened.insert(dir + QLatin1Char('/') + name);
                }
                continue;
            }
            if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO))) {
                addRecursive(dir + QLatin1Char('/') + name);
            }
            relevant = true;
        }
    }
    if (!opened.isEmpty()) {
        Q_EMIT filesOpened(QStringList(opened.cbegin(), opened.cend()));
    }
    if (relevant) {
        Q_EMIT changed();
    }
}

}
