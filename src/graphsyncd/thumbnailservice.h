// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDBusMessage>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>

#include <atomic>
#include <memory>
#include <utility>

namespace GraphSync
{

class Auth;
class GraphClient;

// Thumbnails of online-only files (Files On-Demand placeholders) for Dolphin's previews. Graph
// renders them (images, Office files, PDFs, text); they are fetched on a thread of their own, so a
// D-Bus call never waits for the network. Dolphin keeps them in its own thumbnail cache.
// The worker takes requests from a queue one after another; it has no event queue of its own, so
// a request can never run inside another one's network wait (where it would find the sign-in busy).
class ThumbnailService : public QObject
{
    Q_OBJECT
public:
    struct Request {
        QString placeholderPath;
        QString accountId;
        std::shared_ptr<Auth> auth;
        QString driveId;
        QString itemId; // the online-only file
        int size = 0; // the longest side Dolphin wants
        QDBusMessage message; // answered with the image bytes, empty when there is none
        QElapsedTimer queued; // started by fetch()
    };

    ThumbnailService();
    ~ThumbnailService() override;

    // Thread-safe: queues the request; the D-Bus reply is sent from the worker thread
    void fetch(const Request &request);

private:
    void work();
    QByteArray thumbnail(const Request &request);

    QThread *m_thread;
    QMutex m_mutex;
    QWaitCondition m_wake;
    QQueue<Request> m_queue;
    bool m_stop = false;
    // Set by the destructor: the Graph clients give up on the request they are in
    std::atomic_bool m_cancel = false;
    // Worker thread only: account id -> its sign-in and a Graph client living on this thread
    QHash<QString, std::pair<std::shared_ptr<Auth>, std::shared_ptr<GraphClient>>> m_clients;
};

}
