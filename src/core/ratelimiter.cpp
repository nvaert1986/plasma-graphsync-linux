// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ratelimiter.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QTimer>

#include <algorithm>

namespace GraphSync::RateLimiter
{

namespace
{
// Pacing: each request starts when the previous ones in that direction are "paid for"
struct State {
    QMutex mutex;
    QElapsedTimer clock;
    qint64 downloadRate = 0;
    qint64 uploadRate = 0;
    qint64 downloadFreeAt = 0;
    qint64 uploadFreeAt = 0;
};

State &state()
{
    static State instance;
    if (!instance.clock.isValid()) {
        instance.clock.start();
    }
    return instance;
}

}

void setLimits(qint64 downloadBytesPerSecond, qint64 uploadBytesPerSecond)
{
    State &s = state();
    QMutexLocker locker(&s.mutex);
    s.downloadRate = std::max<qint64>(0, downloadBytesPerSecond);
    s.uploadRate = std::max<qint64>(0, uploadBytesPerSecond);
}

qint64 downloadLimit()
{
    State &s = state();
    QMutexLocker locker(&s.mutex);
    return s.downloadRate;
}

qint64 uploadLimit()
{
    State &s = state();
    QMutexLocker locker(&s.mutex);
    return s.uploadRate;
}

namespace
{
qint64 reserve(qint64 rate, qint64 &freeAt, qint64 bytes)
{
    if (rate <= 0) {
        return 0;
    }
    const qint64 now = state().clock.elapsed();
    const qint64 start = std::max(now, freeAt);
    freeAt = start + bytes * 1000 / rate;
    return start - now;
}

void sleepFor(qint64 waitMs, const std::atomic_bool *cancel)
{
    while (waitMs > 0 && !(cancel && cancel->load())) {
        const int step = int(std::min<qint64>(waitMs, 250));
        QEventLoop loop;
        QTimer::singleShot(step, &loop, &QEventLoop::quit);
        loop.exec();
        waitMs -= step;
    }
}
}

qint64 reserveUpload(qint64 bytes)
{
    State &s = state();
    QMutexLocker locker(&s.mutex);
    return reserve(s.uploadRate, s.uploadFreeAt, bytes);
}

void waitForDownload(qint64 bytes, const std::atomic_bool *cancel)
{
    qint64 waitMs = 0;
    {
        State &s = state();
        QMutexLocker locker(&s.mutex);
        waitMs = reserve(s.downloadRate, s.downloadFreeAt, bytes);
    }
    sleepFor(waitMs, cancel);
}

void waitForUpload(qint64 bytes, const std::atomic_bool *cancel)
{
    sleepFor(reserveUpload(bytes), cancel);
}

}
