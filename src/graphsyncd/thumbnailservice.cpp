// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thumbnailservice.h"
#include "auth.h"
#include "graphclient.h"
#include "graphsync_debug.h"
#include "placeholder.h"

#include <QDBusConnection>

namespace GraphSync
{

namespace
{
// Graph's fixed sizes: medium fits 176 pixels, large 800; custom sizes only come cropped
constexpr int mediumSize = 176;
constexpr qsizetype maxImageBytes = 4 * 1024 * 1024;
// The thumbnailer waits this long for an answer (thumbnailer.cpp): older requests are not fetched
constexpr qint64 callerGivesUpMs = 10000;
}

ThumbnailService::ThumbnailService()
    : m_thread(QThread::create([this] {
        work();
    }))
{
    m_thread->setObjectName(QStringLiteral("thumbnails"));
    m_thread->start();
}

ThumbnailService::~ThumbnailService()
{
    {
        QMutexLocker locker(&m_mutex);
        m_stop = true;
    }
    // Also ends a request that waits for Microsoft (throttling, retries): shutdown never waits for it
    m_cancel = true;
    m_wake.wakeAll();
    m_thread->wait();
    delete m_thread;
}

void ThumbnailService::fetch(const Request &request)
{
    QMutexLocker locker(&m_mutex);
    m_queue.enqueue(request);
    m_queue.last().queued.start();
    m_wake.wakeOne();
}

void ThumbnailService::work()
{
    while (true) {
        Request request;
        // Requests for the same file waiting behind it: answered with the same image
        QList<QDBusMessage> sameFile;
        {
            QMutexLocker locker(&m_mutex);
            while (m_queue.isEmpty() && !m_stop) {
                m_wake.wait(&m_mutex);
            }
            if (m_stop) {
                break;
            }
            request = m_queue.dequeue();
            for (auto it = m_queue.begin(); it != m_queue.end();) {
                if (it->placeholderPath == request.placeholderPath && it->size == request.size) {
                    sameFile.append(it->message);
                    it = m_queue.erase(it);
                } else {
                    ++it;
                }
            }
        }
        // Dolphin gave up on it already (many previews at once): not worth a request to Microsoft
        const QByteArray image = request.queued.hasExpired(callerGivesUpMs) ? QByteArray() : thumbnail(request);
        QDBusConnection::sessionBus().send(request.message.createReply(QVariant::fromValue(image)));
        for (const QDBusMessage &message : std::as_const(sameFile)) {
            QDBusConnection::sessionBus().send(message.createReply(QVariant::fromValue(image)));
        }
    }
    // Requests still waiting get an empty answer; the Graph clients go with this thread
    QMutexLocker locker(&m_mutex);
    while (!m_queue.isEmpty()) {
        const Request request = m_queue.dequeue();
        QDBusConnection::sessionBus().send(request.message.createReply(QVariant::fromValue(QByteArray())));
    }
    m_clients.clear();
}

QByteArray ThumbnailService::thumbnail(const Request &request)
{
    // Only placeholders: real files have their own thumbnailers
    if (!Placeholder::isPlaceholder(request.placeholderPath)) {
        return {};
    }
    auto &client = m_clients[request.accountId];
    if (client.first != request.auth) {
        // A new sign-in for this account (reload, sign-in again): a client over it
        client = {request.auth, std::make_shared<GraphClient>(request.auth.get())};
        client.second->setCancelFlag(&m_cancel);
    }
    const QString size = request.size <= mediumSize ? QStringLiteral("medium") : QStringLiteral("large");
    const GraphResponse response = client.second->get(QStringLiteral("/drives/%1/items/%2/thumbnails/0/%3/content").arg(request.driveId, request.itemId, size));
    if (!response.ok() || response.body.size() > maxImageBytes) {
        // 404: no thumbnail for this kind of file; Dolphin shows the icon
        qCDebug(GRAPHSYNC) << "No thumbnail for" << request.placeholderPath << response.status;
        return {};
    }
    return response.body;
}

}
