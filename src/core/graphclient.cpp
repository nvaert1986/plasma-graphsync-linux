// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "graphclient.h"
#include "auth.h"
#include "graphsync_debug.h"
#include "ratelimiter.h"

#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>

namespace GraphSync
{

namespace
{
constexpr int maxAttempts = 6;
// Without any bytes moving for this long a transfer is given up (and retried). A dropped
// connection gives no error by itself, so this decides how long a dead link goes unnoticed.
constexpr int transferTimeoutMs = 2 * 60 * 1000;
constexpr int maxRetryAfterSeconds = 120;
// Throttling (429/503) is not a failure of the request: it gets more patience
constexpr int maxThrottledAttempts = 20;

// SharePoint throttles per user, so a Retry-After holds back every request of the daemon (all
// roots, all parallel jobs), as Microsoft asks; otherwise the other jobs keep hitting the server,
// earn more 429s and give up
std::atomic<qint64> throttledUntilMs{0};

void noteThrottled(int seconds)
{
    const qint64 until = QDateTime::currentMSecsSinceEpoch() + qint64(seconds) * 1000;
    qint64 current = throttledUntilMs.load();
    while (current < until && !throttledUntilMs.compare_exchange_weak(current, until)) {
    }
}

qint64 throttleWaitMs()
{
    return std::max<qint64>(0, throttledUntilMs.load() - QDateTime::currentMSecsSinceEpoch());
}

bool isThrottled(int status)
{
    return status == 429 || status == 503;
}

// The bearer token only ever goes to Microsoft Graph itself: https, the Graph host, default port
bool isGraph(const QUrl &url)
{
    static const QUrl graph(GraphClient::baseUrl());
    return url.scheme() == QLatin1String("https") && url.host() == graph.host() && (url.port() == -1 || url.port() == 443);
}

// Error texts end up in the journal and the activity list: no query strings, which can hold
// pre-authenticated download addresses (tempauth)
QString withoutQueries(QString text)
{
    static const QRegularExpression query(QStringLiteral("(https?://[^\\s?#]+)[?#]\\S*"));
    return text.replace(query, QStringLiteral("\\1"));
}

bool isRetryableStatus(int status)
{
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

// Moves file content from the reply into the sink and reports it. Only a 2xx answer is the
// file. (The download limit is applied per piece by the caller: Qt buffers a reply regardless of
// its read buffer size, so reading slowly would not slow the connection down.)
void drainReply(QNetworkReply *reply, QIODevice *sink, const std::function<void(qint64)> &observer)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) {
        return;
    }
    const QByteArray chunk = reply->readAll();
    sink->write(chunk);
    if (observer) {
        observer(chunk.size());
    }
}
}

void GraphClient::setTransferObserver(std::function<void(qint64)> observer)
{
    m_observer = std::move(observer);
}

QJsonObject GraphResponse::json() const
{
    return QJsonDocument::fromJson(body).object();
}

QString GraphResponse::graphErrorCode() const
{
    return json().value(QLatin1String("error")).toObject().value(QLatin1String("code")).toString();
}

QString GraphResponse::describe() const
{
    const QJsonObject error = json().value(QLatin1String("error")).toObject();
    if (!error.isEmpty()) {
        return QStringLiteral("HTTP %1 %2: %3")
            .arg(status)
            .arg(error.value(QLatin1String("code")).toString(), error.value(QLatin1String("message")).toString());
    }
    if (status == 0) {
        return withoutQueries(errorString);
    }
    return QStringLiteral("HTTP %1 %2").arg(status).arg(withoutQueries(errorString));
}

GraphClient::GraphClient(Auth *auth)
    : m_auth(auth)
{
    // Redirects are followed by hand so the bearer token never leaves graph.microsoft.com
    m_nam.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}

QString GraphClient::baseUrl()
{
    return QStringLiteral("https://graph.microsoft.com/v1.0");
}

void GraphClient::setCancelFlag(const std::atomic_bool *flag)
{
    m_cancel = flag;
}

GraphResponse GraphClient::get(const QString &pathOrUrl, const HeaderList &headers)
{
    return send("GET", pathOrUrl, {}, headers, true, nullptr);
}

GraphResponse GraphClient::postJson(const QString &path, const QJsonObject &body, const HeaderList &headers)
{
    HeaderList all = headers;
    all.emplaceBack(QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json"));
    return send("POST", path, QJsonDocument(body).toJson(QJsonDocument::Compact), all, true, nullptr);
}

GraphResponse GraphClient::patchJson(const QString &path, const QJsonObject &body, const HeaderList &headers)
{
    HeaderList all = headers;
    all.emplaceBack(QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json"));
    return send("PATCH", path, QJsonDocument(body).toJson(QJsonDocument::Compact), all, true, nullptr);
}

GraphResponse GraphClient::put(const QString &path, const QByteArray &data, const HeaderList &headers)
{
    HeaderList all = headers;
    all.emplaceBack(QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/octet-stream"));
    return send("PUT", path, data, all, true, nullptr);
}

GraphResponse GraphClient::deleteResource(const QString &path, const HeaderList &headers)
{
    return send("DELETE", path, {}, headers, true, nullptr);
}

GraphResponse GraphClient::download(const QString &path, QIODevice *target)
{
    return send("GET", path, {}, {}, true, target);
}

GraphResponse GraphClient::uploadFragment(const QString &uploadUrl, const QByteArray &data, qint64 offset, qint64 total)
{
    const QByteArray range = "bytes " + QByteArray::number(offset) + '-' + QByteArray::number(offset + data.size() - 1) + '/'
        + QByteArray::number(total);
    return send("PUT", uploadUrl, data, {{"Content-Range", range}}, false, nullptr);
}

GraphResponse GraphClient::uploadSessionStatus(const QString &uploadUrl)
{
    return send("GET", uploadUrl, {}, {}, false, nullptr);
}

GraphResponse GraphClient::cancelUploadSession(const QString &uploadUrl)
{
    return send("DELETE", uploadUrl, {}, {}, false, nullptr);
}

QNetworkRequest GraphClient::buildRequest(const QUrl &url, const HeaderList &headers, bool authenticated, QString *tokenError)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(transferTimeoutMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("plasma-graphsync-linux/" GRAPHSYNC_VERSION));
    for (const auto &[name, value] : headers) {
        request.setRawHeader(name, value);
    }
    if (authenticated && isGraph(url)) {
        const QString token = m_auth->accessToken(tokenError);
        if (token.isEmpty()) {
            return {};
        }
        request.setRawHeader("Authorization", "Bearer " + token.toLatin1());
    }
    return request;
}

struct GraphClient::ParallelState {
    QList<ParallelJob> jobs;
    QList<QUrl> urls;
    QList<int> attempts;
    QList<int> redirects;
    QList<bool> refreshed;
    QList<bool> paced;
    qsizetype next = 0;
    int active = 0;
    int concurrency = 1;
    QEventLoop loop;
};

void GraphClient::runParallel(QList<ParallelJob> jobs, int concurrency)
{
    if (jobs.isEmpty()) {
        return;
    }
    ParallelState state;
    state.jobs = std::move(jobs);
    for (const ParallelJob &job : std::as_const(state.jobs)) {
        state.urls.append(QUrl(job.pathOrUrl.startsWith(QLatin1String("https://")) ? job.pathOrUrl : baseUrl() + job.pathOrUrl));
        state.attempts.append(0);
        state.redirects.append(0);
        state.refreshed.append(false);
        state.paced.append(false);
    }
    state.concurrency = std::max(1, concurrency);
    pumpParallel(&state);
    if (state.active > 0) {
        state.loop.exec();
    }
}

void GraphClient::pumpParallel(ParallelState *state)
{
    while (state->active < state->concurrency && state->next < state->jobs.size()) {
        startParallelJob(state, state->next++);
    }
    if (state->active == 0 && state->next >= state->jobs.size()) {
        state->loop.quit();
    }
}

bool GraphClient::startParallelJob(ParallelState *state, qsizetype index)
{
    ParallelJob &job = state->jobs[index];
    if (m_cancel && m_cancel->load()) {
        job.done({0, {}, QStringLiteral("cancelled")});
        return false;
    }
    // Throttled: wait until SharePoint's Retry-After has passed, without counting an attempt
    if (const qint64 waitMs = throttleWaitMs(); waitMs > 0) {
        ++state->active; // the slot stays taken while waiting
        QTimer::singleShot(waitMs, &state->loop, [this, state, index] {
            --state->active;
            startParallelJob(state, index);
            pumpParallel(state);
        });
        return true;
    }
    // Upload limit: wait for this body's turn without blocking the other transfers
    if (job.verb == "PUT" && !job.body.isEmpty() && !state->paced.at(index)) {
        const qint64 delayMs = RateLimiter::reserveUpload(job.body.size());
        if (delayMs > 0) {
            state->paced[index] = true;
            ++state->active; // the slot stays taken while waiting
            QTimer::singleShot(delayMs, &state->loop, [this, state, index] {
                --state->active;
                startParallelJob(state, index);
                pumpParallel(state);
            });
            return true;
        }
    }
    state->paced[index] = false;
    ++state->attempts[index];
    QString tokenError;
    const QNetworkRequest request = buildRequest(state->urls.at(index), job.headers, true, &tokenError);
    if (request.url().isEmpty()) {
        job.done({0, {}, QStringLiteral("no access token: ") + tokenError});
        return false;
    }
    if (job.sink) {
        job.sink->resize(0);
        job.sink->seek(0);
    }
    ++state->active;
    QNetworkReply *reply = m_nam.sendCustomRequest(request, job.verb, job.body);
    if (job.sink) {
        QFile *sink = job.sink;
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [this, reply, sink] {
            drainReply(reply, sink, m_observer);
        });
    }
    QObject::connect(reply, &QNetworkReply::finished, reply, [this, state, index, reply] {
        finishParallelReply(state, index, reply);
    });
    return true;
}

void GraphClient::finishParallelReply(ParallelState *state, qsizetype index, QNetworkReply *reply)
{
    reply->deleteLater();
    --state->active;
    ParallelJob &job = state->jobs[index];

    GraphResponse response;
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && response.ok()) {
        // The server answered 200 but the transfer broke off (connection dropped, timeout):
        // a network failure to retry, never a finished download
        response.status = 0;
    }
    if (job.sink && response.ok()) {
        const QByteArray rest = reply->readAll();
        job.sink->write(rest);
        if (m_observer) {
            m_observer(rest.size());
        }
    } else {
        response.body = reply->readAll();
    }
    if (m_observer && job.verb == "PUT" && response.ok()) {
        m_observer(job.body.size());
    }
    response.errorString = reply->errorString();
    const QUrl location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const QNetworkReply::NetworkError netError = reply->error();
    const QByteArray retryAfter = reply->rawHeader("Retry-After");

    bool restart = false;
    int delayMs = 0;
    // Redirects only to https: an https-to-http hop would expose whatever it carries
    if (response.status >= 300 && response.status < 400 && location.isValid() && state->redirects.at(index) < 5
        && state->urls.at(index).resolved(location).scheme() == QLatin1String("https")) {
        ++state->redirects[index];
        --state->attempts[index];
        state->urls[index] = state->urls.at(index).resolved(location);
        restart = true;
    } else if (response.status == 401 && !state->refreshed.at(index) && isGraph(state->urls.at(index))) {
        state->refreshed[index] = true;
        QString tokenError;
        m_auth->accessToken(&tokenError, true);
        restart = true;
    } else if (state->attempts.at(index) < (isThrottled(response.status) ? maxThrottledAttempts : maxAttempts)
               && (isRetryableStatus(response.status)
                   || (response.status == 0 && netError != QNetworkReply::OperationCanceledError && !(m_cancel && m_cancel->load())))) {
        bool ok = false;
        const int serverWait = retryAfter.toInt(&ok);
        delayMs = (ok ? std::clamp(serverWait, 1, maxRetryAfterSeconds) : std::min(1 << state->attempts.at(index), 60)) * 1000;
        if (isThrottled(response.status)) {
            noteThrottled(delayMs / 1000);
            qCInfo(GRAPHSYNC).noquote() << QStringLiteral("Throttled by Microsoft 365 (HTTP %1), pausing requests for %2 s").arg(response.status).arg(delayMs / 1000);
        }
        restart = true;
        if (response.status == 0) {
            // The kept-alive connection may be dead (network dropped): retry on a fresh one,
            // or every retry waits for the timeout again on the same dead socket
            m_nam.clearConnectionCache();
        }
    }

    if (restart) {
        if (delayMs > 0) {
            ++state->active; // keep the loop alive while waiting
            QTimer::singleShot(delayMs, &state->loop, [this, state, index] {
                --state->active;
                startParallelJob(state, index);
                pumpParallel(state);
            });
        } else {
            startParallelJob(state, index);
        }
    } else {
        job.done(response);
    }
    pumpParallel(state);
}

GraphResponse GraphClient::send(const QByteArray &verb, const QString &pathOrUrl, const QByteArray &body, const HeaderList &headers,
                                bool authenticated, QIODevice *sink)
{
    QUrl url(pathOrUrl.startsWith(QLatin1String("https://")) ? pathOrUrl : baseUrl() + pathOrUrl);
    bool refreshedAfter401 = false;
    int redirects = 0;
    GraphResponse response;

    for (int attempt = 1; attempt <= maxThrottledAttempts; ++attempt) {
        if (m_cancel && m_cancel->load()) {
            response = {0, {}, QStringLiteral("cancelled")};
            return response;
        }
        for (qint64 waitMs = throttleWaitMs(); waitMs > 0 && !(m_cancel && m_cancel->load()); waitMs = throttleWaitMs()) {
            QThread::msleep(std::min<qint64>(waitMs, 100));
        }

        QNetworkRequest request(url);
        request.setTransferTimeout(transferTimeoutMs);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("plasma-graphsync-linux/" GRAPHSYNC_VERSION));
        for (const auto &[name, value] : headers) {
            request.setRawHeader(name, value);
        }
        if (authenticated && isGraph(url)) {
            QString tokenError;
            const QString token = m_auth->accessToken(&tokenError);
            if (token.isEmpty()) {
                return {0, {}, QStringLiteral("no access token: ") + tokenError};
            }
            request.setRawHeader("Authorization", "Bearer " + token.toLatin1());
        }
        if (sink && sink->isWritable() && sink->pos() > 0) {
            // A retried download starts over
            sink->seek(0);
        }
        if (verb == "PUT" && !body.isEmpty()) {
            RateLimiter::waitForUpload(body.size(), m_cancel);
        }

        QNetworkReply *reply = m_nam.sendCustomRequest(request, verb, body);
        if (sink) {
            // Only the final 2xx answer is the file; redirect and error bodies are not
            QObject::connect(reply, &QNetworkReply::readyRead, reply, [this, reply, sink] {
                drainReply(reply, sink, m_observer);
            });
        }
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && response.ok()) {
            // Answered 200, then broke off: retry instead of keeping half a file
            response.status = 0;
        }
        if (sink && response.ok()) {
            const QByteArray rest = reply->readAll();
            sink->write(rest);
            if (m_observer) {
                m_observer(rest.size());
            }
            response.body.clear();
        } else {
            response.body = reply->readAll();
        }
        if (m_observer && verb == "PUT" && response.ok()) {
            m_observer(body.size());
        }
        response.errorString = reply->errorString();
        const QByteArray retryAfter = reply->rawHeader("Retry-After");
        const QUrl location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const QNetworkReply::NetworkError netError = reply->error();
        reply->deleteLater();

        if (response.status >= 300 && response.status < 400 && location.isValid() && redirects < 5
            && url.resolved(location).scheme() == QLatin1String("https")) {
            ++redirects;
            --attempt; // following a redirect is not a failed attempt
            url = url.resolved(location);
            continue;
        }

        if (response.status == 401 && authenticated && isGraph(url) && !refreshedAfter401) {
            refreshedAfter401 = true;
            qCInfo(GRAPHSYNC) << "Graph returned 401, refreshing the access token once";
            QString tokenError;
            m_auth->accessToken(&tokenError, true);
            continue;
        }

        // Without an HTTP status the request never got an answer (timeouts, DNS, a server closing
        // the HTTP/2 connection with GOAWAY, ...): retry, unless we cancelled it ourselves
        const bool transient = isRetryableStatus(response.status)
            || (response.status == 0 && netError != QNetworkReply::OperationCanceledError && !(m_cancel && m_cancel->load()));
        if (!transient || attempt >= (isThrottled(response.status) ? maxThrottledAttempts : maxAttempts)) {
            return response;
        }

        if (response.status == 0) {
            // As in the parallel path: never retry on a connection that may be dead
            m_nam.clearConnectionCache();
        }
        int waitSeconds = std::min(1 << attempt, 60);
        bool ok = false;
        const int serverWait = retryAfter.toInt(&ok);
        if (ok) {
            waitSeconds = std::clamp(serverWait, 1, maxRetryAfterSeconds);
        }
        if (isThrottled(response.status)) {
            noteThrottled(waitSeconds);
        }
        qCInfo(GRAPHSYNC) << verb << url.path() << "->" << (response.status ? QString::number(response.status) : response.errorString)
                     << "retrying in" << waitSeconds << "s";
        for (int waited = 0; waited < waitSeconds * 10; ++waited) {
            if (m_cancel && m_cancel->load()) {
                break;
            }
            QThread::msleep(100);
        }
    }
    return response;
}

}
