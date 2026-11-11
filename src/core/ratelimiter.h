// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QtGlobal>

#include <atomic>

// Bandwidth limits for all transfers of the process together (every sync root's engine runs in
// its own thread). Thread-safe. A limit of 0 means no limit.
namespace GraphSync::RateLimiter
{

void setLimits(qint64 downloadBytesPerSecond, qint64 uploadBytesPerSecond);
qint64 downloadLimit();
qint64 uploadLimit();

// Downloads: blocks (local event loop) until the next `bytes` may be fetched. Callers fetch in
// pieces of about a second of the limit (HTTP Range), so this paces the connection itself.
void waitForDownload(qint64 bytes, const std::atomic_bool *cancel);

// Uploads: reserves `bytes` and returns how many milliseconds to wait before sending them
qint64 reserveUpload(qint64 bytes);
// Uploads: blocks (running a local event loop) until `bytes` may be sent, so uploads average
// out at the limit. Returns early when cancel is set.
void waitForUpload(qint64 bytes, const std::atomic_bool *cancel);

}
