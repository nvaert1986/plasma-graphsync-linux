// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QString>

class QSocketNotifier;

namespace GraphSync
{

// Recursive inotify watch on a sync root. Emits changed() for any relevant event; the
// receiver debounces and runs a sync, which rescans anyway, so no event details are passed.
class LocalWatcher : public QObject
{
    Q_OBJECT
public:
    explicit LocalWatcher(const QString &root, QObject *parent = nullptr);
    ~LocalWatcher() override;

    bool isActive() const;

Q_SIGNALS:
    void changed();
    // The root itself was removed or moved away: syncing must stop, never delete remotely
    void rootLost();

private:
    void addRecursive(const QString &path);
    void readEvents();

    QString m_root;
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QHash<int, QString> m_watches;
};

}
