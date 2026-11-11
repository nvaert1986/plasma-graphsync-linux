// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QPair>
#include <QString>

#include <atomic>
#include <functional>

class QFile;
class QIODevice;
class QNetworkReply;

namespace GraphSync
{

class Auth;

struct GraphResponse {
    int status = 0; // HTTP status, 0 when the request never got an answer
    QByteArray body;
    QString errorString;

    bool ok() const
    {
        return status >= 200 && status < 300;
    }
    QJsonObject json() const;
    // Graph error code ("itemNotFound", "notAllowed", ...) and message, when present
    QString graphErrorCode() const;
    QString describe() const;
};

using HeaderList = QList<QPair<QByteArray, QByteArray>>;

// One request for GraphClient::runParallel. done() runs in the calling thread when the job
// has finished, including after retries and redirects.
struct ParallelJob {
    QByteArray verb = "GET";
    QString pathOrUrl;
    QByteArray body;
    HeaderList headers;
    QFile *sink = nullptr; // receives the body of a 2xx answer (downloads)
    std::function<void(const GraphResponse &)> done;
};

// Blocking Microsoft Graph client. Each instance owns its network manager and must only be
// used from the thread that created it (a sync worker or a command-line tool). Calls run a
// local event loop until the reply is complete.
class GraphClient
{
public:
    explicit GraphClient(Auth *auth);

    static QString baseUrl();

    // pathOrUrl is either a Graph path ("/drives/x/root") or an absolute URL (nextLink etc.)
    GraphResponse get(const QString &pathOrUrl, const HeaderList &headers = {});
    GraphResponse postJson(const QString &path, const QJsonObject &body, const HeaderList &headers = {});
    GraphResponse patchJson(const QString &path, const QJsonObject &body, const HeaderList &headers = {});
    GraphResponse put(const QString &path, const QByteArray &data, const HeaderList &headers = {});
    GraphResponse deleteResource(const QString &path, const HeaderList &headers = {});

    // Streams the body of a GET into target (follows the redirect to the download host).
    GraphResponse download(const QString &path, QIODevice *target);

    // PUT one fragment to a pre-authenticated upload-session URL; must not carry our token.
    GraphResponse uploadFragment(const QString &uploadUrl, const QByteArray &data, qint64 offset, qint64 total);
    // State (nextExpectedRanges) and cancellation of an upload session; no token either.
    GraphResponse uploadSessionStatus(const QString &uploadUrl);
    GraphResponse cancelUploadSession(const QString &uploadUrl);

    // Runs jobs with up to `concurrency` requests in flight, with the same retry, redirect and
    // token rules as the blocking calls. Returns when every job is done (or cancelled).
    void runParallel(QList<ParallelJob> jobs, int concurrency);

    // Set from another thread to make long transfers give up early (pause, shutdown).
    void setCancelFlag(const std::atomic_bool *flag);
    // Called with the number of bytes each time file content was received or an upload body
    // was accepted (transfer progress)
    void setTransferObserver(std::function<void(qint64)> observer);

private:
    struct ParallelState;
    bool startParallelJob(ParallelState *state, qsizetype index);
    void pumpParallel(ParallelState *state);
    void finishParallelReply(ParallelState *state, qsizetype index, QNetworkReply *reply);
    QNetworkRequest buildRequest(const QUrl &url, const HeaderList &headers, bool authenticated, QString *tokenError);

    GraphResponse send(const QByteArray &verb, const QString &pathOrUrl, const QByteArray &body, const HeaderList &headers,
                       bool authenticated, QIODevice *sink);

    Auth *m_auth;
    QNetworkAccessManager m_nam;
    const std::atomic_bool *m_cancel = nullptr;
    std::function<void(qint64)> m_observer;
};

}
