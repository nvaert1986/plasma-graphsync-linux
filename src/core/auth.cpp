// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "auth.h"
#include "graphsync_debug.h"

#include <qt6keychain/keychain.h>

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QOAuth2AuthorizationCodeFlow>
#include <QOAuthHttpServerReplyHandler>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <utility>

namespace GraphSync
{

namespace
{
const QString keychainService = QStringLiteral("plasma-graphsync-linux");
constexpr int loginTimeoutMs = 5 * 60 * 1000;

// Blocking form POST to the identity platform; the JSON answer (also for errors)
QJsonObject postForm(const QUrl &url, const QUrlQuery &form, QString *networkError)
{
    QNetworkAccessManager nam;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setTransferTimeout(60 * 1000);
    QNetworkReply *reply = nam.post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    if (networkError) {
        *networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    }
    return QJsonDocument::fromJson(reply->readAll()).object();
}

QJsonObject jwtClaims(const QString &jwt)
{
    const QStringList parts = jwt.split(QLatin1Char('.'));
    if (parts.size() < 2) {
        return {};
    }
    const QByteArray payload = QByteArray::fromBase64(parts.at(1).toLatin1(), QByteArray::Base64UrlEncoding);
    return QJsonDocument::fromJson(payload).object();
}

// QtKeychain jobs run on one thread of their own, one after the other (as graphcore does). Jobs
// started on several threads at once (a sync thread storing a renewed token while the main thread
// reads or deletes one) left QtKeychain's job queue waiting forever, and its "finished" for a job
// on another thread could arrive after the job was gone. Callers simply wait for the result; they
// run no event loop of their own for it, so nothing can re-enter them meanwhile.
QObject *keychainContext()
{
    static QObject *context = [] {
        auto *thread = new QThread;
        thread->setObjectName(QStringLiteral("keychain"));
        thread->start();
        auto *object = new QObject;
        object->moveToThread(thread);
        return object;
    }();
    return context;
}

// Runs `work` on the keychain thread and waits for it
void onKeychainThread(const std::function<void()> &work)
{
    QObject *context = keychainContext();
    if (QThread::currentThread() == context->thread()) {
        work();
        return;
    }
    QMetaObject::invokeMethod(context, work, Qt::BlockingQueuedConnection);
}

// A QtKeychain job to completion, in a local event loop on the keychain thread
template<typename Job>
bool runKeychainJob(Job *job, QString *error, QKeychain::Error *code = nullptr)
{
    QEventLoop loop;
    QObject::connect(job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job->setAutoDelete(false);
    job->start();
    loop.exec();
    const bool ok = job->error() == QKeychain::NoError;
    if (!ok && error) {
        *error = job->errorString();
    }
    if (code) {
        *code = job->error();
    }
    return ok;
}

// The three operations, each job made, run and gone on the keychain thread
bool keychainRead(const QString &service, const QString &key, QString *data, QString *error, QKeychain::Error *code = nullptr)
{
    bool ok = false;
    QString failure;
    onKeychainThread([&] {
        QKeychain::ReadPasswordJob job(service);
        job.setKey(key);
        ok = runKeychainJob(&job, &failure, code);
        if (ok) {
            *data = job.textData();
        }
    });
    if (!ok && error) {
        *error = failure;
    }
    return ok;
}

bool keychainWrite(const QString &service, const QString &key, const QString &data, QString *error)
{
    bool ok = false;
    QString failure;
    onKeychainThread([&] {
        QKeychain::WritePasswordJob job(service);
        job.setKey(key);
        job.setTextData(data);
        ok = runKeychainJob(&job, &failure);
    });
    if (!ok && error) {
        *error = failure;
    }
    return ok;
}

void keychainDelete(const QString &service, const QString &key)
{
    onKeychainThread([&] {
        QKeychain::DeletePasswordJob job(service);
        job.setKey(key);
        QString ignored;
        runKeychainJob(&job, &ignored);
    });
}
}

// m_mutex for one operation. It never waits on itself: an operation re-entered on the thread that
// holds it (from a local event loop: a KWallet read, a token refresh) does not get it, nor does a
// NoWait operation while another thread holds it. What such operations asked for (new settings, a
// sign-out) is done here as the holder lets go.
class Auth::TokenLock
{
public:
    enum Mode {
        Wait,
        NoWait,
    };
    TokenLock(Auth *auth, Mode mode)
        : m_auth(auth)
    {
        if (auth->m_tokenOwner.load() == QThread::currentThreadId()) {
            return;
        }
        if (mode == Wait) {
            auth->m_mutex.lock();
            m_locked = true;
        } else {
            m_locked = auth->m_mutex.tryLock();
        }
        if (m_locked) {
            auth->m_tokenOwner.store(QThread::currentThreadId());
        }
    }
    ~TokenLock()
    {
        if (m_locked) {
            m_auth->applyPendingLocked();
            m_auth->m_tokenOwner.store(nullptr);
            m_auth->m_mutex.unlock();
        }
    }
    Q_DISABLE_COPY_MOVE(TokenLock)
    bool locked() const
    {
        return m_locked;
    }

private:
    Auth *m_auth;
    bool m_locked = false;
};

Auth::Auth(const AuthConfig &config)
    : m_config(config)
{
}

bool Auth::isConfigured() const
{
    const AuthConfig config = this->config();
    // The parts end up in a KWallet key "tenant:client:user": none may hold the separator
    return isGuid(config.clientId) && !config.tenantId.isEmpty() && !config.tenantId.contains(QLatin1Char(':'))
        && (config.objectId.isEmpty() || isGuid(config.objectId));
}

QString Auth::scopes()
{
    return QStringLiteral("Files.ReadWrite.All Sites.ReadWrite.All User.Read offline_access openid profile");
}

namespace
{
// Tests only (setLoginServerForTesting): a loopback server answering in place of Microsoft
QString &testLoginServer()
{
    static QString server;
    return server;
}
}

void Auth::setLoginServerForTesting(const QUrl &server)
{
    // Never another host than this machine: a sign-in must not be sent anywhere else
    const bool loopback = server.scheme() == QLatin1String("http") && (server.host() == QLatin1String("127.0.0.1") || server.host() == QLatin1String("localhost"));
    testLoginServer() = loopback ? server.toString(QUrl::StripTrailingSlash) : QString();
}

void Auth::setRefreshTokenForTesting(const QString &token)
{
    TokenLock lock(this, TokenLock::Wait);
    setRefreshToken(token);
}

QString Auth::authority() const
{
    const QString server = testLoginServer().isEmpty() ? QStringLiteral("https://login.microsoftonline.com") : testLoginServer();
    return server + QStringLiteral("/%1/oauth2/v2.0").arg(m_config.tenantId);
}

QString Auth::keychainKeyFor(const AuthConfig &config)
{
    const QString registration = config.tenantId + QLatin1Char(':') + config.clientId;
    return config.objectId.isEmpty() ? registration : registration + QLatin1Char(':') + config.objectId;
}

QString Auth::keychainKey() const
{
    return keychainKeyFor(m_config);
}

bool Auth::interactiveLogin(QString *error, const QString &loginHint)
{
    if (!isConfigured()) {
        *error = QStringLiteral("client id or tenant id not configured (graphsyncctl configure)");
        return false;
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
    *error = QStringLiteral("signing in through the browser needs Qt 6.9 or newer; sign in with a code instead (graphsyncctl login device)");
    return false;
#else
    QOAuth2AuthorizationCodeFlow flow;
    flow.setAuthorizationUrl(QUrl(authority() + QStringLiteral("/authorize")));
    flow.setTokenUrl(QUrl(authority() + QStringLiteral("/token")));
    flow.setClientIdentifier(m_config.clientId);
    QSet<QByteArray> scopeTokens;
    for (const QString &scope : scopes().split(QLatin1Char(' '))) {
        scopeTokens.insert(scope.toLatin1());
    }
    flow.setRequestedScopeTokens(scopeTokens);
    flow.setPkceMethod(QOAuth2AuthorizationCodeFlow::PkceMethod::S256);
    flow.setModifyParametersFunction([loginHint](QAbstractOAuth::Stage stage, QMultiMap<QString, QVariant> *parameters) {
        if (stage == QAbstractOAuth::Stage::RequestingAuthorization) {
            parameters->insert(QStringLiteral("prompt"), QStringLiteral("select_account"));
            if (!loginHint.isEmpty()) {
                parameters->insert(QStringLiteral("login_hint"), loginHint);
            }
        }
    });

    // Entra ignores the port for http://localhost redirect URIs of public clients, so any
    // free port works; it must be "localhost" because that is what the app registration lists.
    auto *handler = new QOAuthHttpServerReplyHandler(QHostAddress::LocalHost, 0, &flow);
    handler->setCallbackHost(QStringLiteral("localhost"));
    if (!handler->isListening()) {
        *error = QStringLiteral("could not open a local port for the sign-in redirect");
        return false;
    }
    flow.setReplyHandler(handler);

    QEventLoop loop;
    QString failure;
    QObject::connect(&flow, &QAbstractOAuth::authorizeWithBrowser, &flow, [](const QUrl &url) {
        if (!QProcess::startDetached(QStringLiteral("xdg-open"), {url.toString(QUrl::FullyEncoded)})) {
            qCWarning(GRAPHSYNC) << "Could not start xdg-open; open this URL manually:" << url.toString(QUrl::FullyEncoded);
        }
    });
    QObject::connect(&flow, &QAbstractOAuth::granted, &loop, &QEventLoop::quit);
    QObject::connect(&flow, &QAbstractOAuth2::serverReportedErrorOccurred, &loop,
                     [&](const QString &code, const QString &description, const QUrl &) {
                         failure = code + QStringLiteral(": ") + description;
                         loop.quit();
                     });
    QObject::connect(&flow, &QAbstractOAuth::requestFailed, &loop, [&](QAbstractOAuth::Error e) {
        failure = QStringLiteral("sign-in request failed (QAbstractOAuth::Error %1)").arg(static_cast<int>(e));
        loop.quit();
    });
    QTimer::singleShot(loginTimeoutMs, &loop, [&] {
        failure = QStringLiteral("no sign-in completed within five minutes");
        loop.quit();
    });

    flow.grant();
    loop.exec();

    if (flow.status() != QAbstractOAuth::Status::Granted || flow.refreshToken().isEmpty()) {
        *error = failure.isEmpty() ? QStringLiteral("sign-in did not complete") : failure;
        return false;
    }

    TokenLock lock(this, TokenLock::Wait);
    if (!lock.locked()) {
        *error = QStringLiteral("the sign-in is busy; try again");
        return false;
    }
    return completeSignIn(flow.token(), flow.expirationAt(), flow.refreshToken(), flow.idToken(), error);
#endif
}

bool Auth::deviceCodeLogin(const std::function<void(const QString &, const QString &)> &show, const std::atomic_bool *cancel, QString *error)
{
    QUrlQuery request;
    request.addQueryItem(QStringLiteral("client_id"), m_config.clientId);
    request.addQueryItem(QStringLiteral("scope"), scopes());
    QString networkError;
    const QJsonObject code = postForm(QUrl(authority() + QStringLiteral("/devicecode")), request, &networkError);
    const QString deviceCode = code.value(QLatin1String("device_code")).toString();
    if (deviceCode.isEmpty()) {
        const QString description = code.value(QLatin1String("error_description")).toString().section(QLatin1Char('\n'), 0, 0);
        *error = description.isEmpty() ? networkError : description;
        return false;
    }
    show(code.value(QLatin1String("user_code")).toString(), code.value(QLatin1String("verification_uri")).toString());

    int intervalSeconds = std::max(1, code.value(QLatin1String("interval")).toInt(5));
    QElapsedTimer age;
    age.start();
    const qint64 lifetimeMs = qint64(code.value(QLatin1String("expires_in")).toInt(900)) * 1000;
    while (age.elapsed() < lifetimeMs) {
        // Wait in small steps so cancelling is quick
        for (int waited = 0; waited < intervalSeconds * 1000; waited += 250) {
            if (cancel && cancel->load()) {
                *error = QStringLiteral("cancelled");
                return false;
            }
            QEventLoop pause;
            QTimer::singleShot(250, &pause, &QEventLoop::quit);
            pause.exec();
        }
        QUrlQuery poll;
        poll.addQueryItem(QStringLiteral("client_id"), m_config.clientId);
        poll.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("urn:ietf:params:oauth:grant-type:device_code"));
        poll.addQueryItem(QStringLiteral("device_code"), deviceCode);
        const QJsonObject answer = postForm(QUrl(authority() + QStringLiteral("/token")), poll, &networkError);
        if (answer.contains(QLatin1String("access_token"))) {
            TokenLock lock(this, TokenLock::Wait);
            if (!lock.locked()) {
                *error = QStringLiteral("the sign-in is busy; try again");
                return false;
            }
            return completeSignIn(answer.value(QLatin1String("access_token")).toString(),
                                  QDateTime::currentDateTimeUtc().addSecs(answer.value(QLatin1String("expires_in")).toInt(3600)),
                                  answer.value(QLatin1String("refresh_token")).toString(),
                                  answer.value(QLatin1String("id_token")).toString(),
                                  error);
        }
        const QString state = answer.value(QLatin1String("error")).toString();
        if (state == QLatin1String("authorization_pending")) {
            continue;
        }
        if (state == QLatin1String("slow_down")) {
            intervalSeconds += 5;
            continue;
        }
        if (state.isEmpty() && !networkError.isEmpty()) {
            // A network hiccup while waiting; try again
            continue;
        }
        const QString description = answer.value(QLatin1String("error_description")).toString().section(QLatin1Char('\n'), 0, 0);
        *error = description.isEmpty() ? state : description;
        return false;
    }
    *error = QStringLiteral("the code expired before sign-in completed");
    return false;
}

QString Auth::legacyKey() const
{
    // Recorded at migration; older records only name the user
    const QString recorded = Config::legacyTokenKey();
    return recorded.isEmpty() ? keychainKeyFor({m_config.clientId, m_config.tenantId, {}}) : recorded;
}

QString Auth::noUserError()
{
    return QStringLiteral("Microsoft did not say which user signed in (no usable ID token); try again");
}

AccountInfo Auth::signedInUserFrom(const QString &idToken)
{
    const QJsonObject claims = jwtClaims(idToken);
    AccountInfo user;
    user.objectId = claims.value(QLatin1String("oid")).toString().toLower();
    user.tenantId = claims.value(QLatin1String("tid")).toString().toLower();
    user.userPrincipalName = claims.value(QLatin1String("preferred_username")).toString();
    user.displayName = claims.value(QLatin1String("name")).toString();
    if (!isGuid(user.objectId) || !isGuid(user.tenantId)) {
        // No usable ID token: the sign-in fails (a token without its user could end up with
        // another account)
        return {};
    }
    return user;
}

bool Auth::completeSignIn(const QString &accessToken, const QDateTime &expiresAt, const QString &refreshToken, const QString &idToken, QString *error)
{
    // Nothing changes until the new user is known and its sign-in is stored: on any failure this
    // Auth keeps the previous account's settings and tokens as they were
    AccountInfo user = signedInUserFrom(idToken);
    if (user.objectId.isEmpty()) {
        *error = noUserError();
        return false;
    }
    if (refreshToken.isEmpty()) {
        *error = QStringLiteral("no refresh token in the answer");
        return false;
    }
    // From now on the user's own tenant (the same one, or the tenant behind a typed domain)
    const AuthConfig target{m_config.clientId, user.tenantId, user.objectId};
    if (!writeRefreshTokenFor(target, refreshToken, error)) {
        return false;
    }
    {
        QMutexLocker configLocker(&m_configMutex);
        m_config = target;
    }
    m_accessToken = accessToken;
    m_expiresAt = expiresAt;
    setRefreshToken(refreshToken);
    user.clientId = target.clientId;
    registerSignedInUser(user);
    return true;
}

void Auth::registerSignedInUser(const AccountInfo &user)
{
    if (!user.objectId.isEmpty()) {
        // The organisation name comes from Graph later; a known account keeps its own
        Config::registerAccount(user);
    }
}

QString Auth::accessToken(QString *error, bool forceRefresh)
{
    TokenLock lock(this, TokenLock::Wait);
    if (!lock.locked()) {
        // Re-entered while this thread refreshes or reads KWallet for the same account
        if (error) {
            *error = QStringLiteral("the sign-in is busy; try again");
        }
        return {};
    }
    const bool fresh = !m_accessToken.isEmpty() && m_expiresAt.isValid()
        && QDateTime::currentDateTimeUtc().secsTo(m_expiresAt) > 300;
    if (fresh && !forceRefresh) {
        return m_accessToken;
    }
    if (!refresh(error)) {
        return {};
    }
    return m_accessToken;
}

bool Auth::refresh(QString *error)
{
    if (m_refreshToken.isEmpty()) {
        setRefreshToken(readRefreshToken(error));
        if (m_refreshToken.isEmpty()) {
            if (error && error->isEmpty()) {
                *error = QStringLiteral("not signed in (graphsyncctl login)");
            }
            return false;
        }
    }

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("client_id"), m_config.clientId);
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    form.addQueryItem(QStringLiteral("refresh_token"), m_refreshToken);
    form.addQueryItem(QStringLiteral("scope"), scopes());

    QNetworkAccessManager nam;
    QNetworkRequest request(QUrl(authority() + QStringLiteral("/token")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setTransferTimeout(60 * 1000);
    QNetworkReply *reply = nam.post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();

    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    if (reply->error() != QNetworkReply::NoError || !body.contains(QLatin1String("access_token"))) {
        const QString description = body.value(QLatin1String("error_description")).toString().section(QLatin1Char('\n'), 0, 0);
        // Callers tell these apart: only "sign-in expired" means signing in again; no answer at
        // all (offline, no DNS) is a connection problem that goes away by itself
        if (body.value(QLatin1String("error")).toString() == QLatin1String("invalid_grant")) {
            // The refresh token is dead (revoked, expired, password change)
            setRefreshToken({});
            *error = QStringLiteral("sign-in expired: ") + description;
        } else if (body.isEmpty()) {
            *error = QStringLiteral("no connection: ") + reply->errorString();
        } else {
            *error = description.isEmpty() ? reply->errorString() : description;
        }
        return false;
    }

    m_accessToken = body.value(QLatin1String("access_token")).toString();
    m_expiresAt = QDateTime::currentDateTimeUtc().addSecs(body.value(QLatin1String("expires_in")).toInt(3600));
    const QString rotated = body.value(QLatin1String("refresh_token")).toString();
    if (!rotated.isEmpty() && rotated != m_refreshToken) {
        setRefreshToken(rotated);
        QString writeError;
        if (!writeRefreshToken(rotated, &writeError)) {
            qCWarning(GRAPHSYNC) << "Could not store the rotated refresh token:" << writeError;
        }
    }
    return true;
}

bool Auth::hasStoredRefreshToken()
{
    TokenLock lock(this, TokenLock::NoWait);
    if (!lock.locked()) {
        // In use (a refresh, a KWallet read): never wait for it here (D-Bus handlers, the UI)
        return m_haveRefreshToken.load();
    }
    if (!m_refreshToken.isEmpty()) {
        return true;
    }
    QString error;
    setRefreshToken(readRefreshToken(&error));
    return !m_refreshToken.isEmpty();
}

void Auth::setRefreshToken(const QString &token)
{
    m_refreshToken = token;
    m_haveRefreshToken.store(!token.isEmpty());
}

void Auth::applyPendingLocked()
{
    bool logoutNow = false;
    std::optional<AuthConfig> config;
    {
        QMutexLocker configLocker(&m_configMutex);
        logoutNow = std::exchange(m_pendingLogout, false);
        config = std::exchange(m_pendingConfig, std::nullopt);
    }
    if (logoutNow) {
        logoutLocked();
    }
    if (config) {
        {
            QMutexLocker configLocker(&m_configMutex);
            m_config = *config;
        }
        m_accessToken.clear();
        setRefreshToken({});
        m_expiresAt = {};
    }
}

AuthConfig Auth::config() const
{
    QMutexLocker locker(&m_configMutex);
    return m_config;
}

void Auth::setConfig(const AuthConfig &config)
{
    TokenLock lock(this, TokenLock::NoWait);
    if (!lock.locked()) {
        // In use: the holder switches when it is done (never waiting here, see TokenLock)
        QMutexLocker configLocker(&m_configMutex);
        m_pendingConfig = config;
        return;
    }
    {
        QMutexLocker configLocker(&m_configMutex);
        m_config = config;
        m_pendingConfig.reset();
    }
    m_accessToken.clear();
    setRefreshToken({});
    m_expiresAt = {};
}

namespace
{
// Moves a refresh token between two KWallet entries; the old one goes once the new one is written
bool moveKeychainEntry(const QString &fromService, const QString &fromKey, const QString &toService, const QString &toKey, QString *error)
{
    QString token;
    if (!keychainRead(fromService, fromKey, &token, error) || !keychainWrite(toService, toKey, token, error)) {
        return false;
    }
    if (fromService != toService || fromKey != toKey) {
        keychainDelete(fromService, fromKey);
    }
    return true;
}
}

bool Auth::importLegacyToken(const AuthConfig &legacy, const AuthConfig &target, QString *error)
{
    return moveKeychainEntry(QStringLiteral("plasma-onedrive-linux"), keychainKeyFor(legacy), keychainService, keychainKeyFor(target), error);
}

bool Auth::moveToken(const AuthConfig &from, const AuthConfig &to, QString *error)
{
    return moveKeychainEntry(keychainService, keychainKeyFor(from), keychainService, keychainKeyFor(to), error);
}

AuthConfig Auth::normalized(const AuthConfig &config)
{
    return {config.clientId.trimmed().toLower(), config.tenantId.trimmed().toLower(), config.objectId.trimmed().toLower()};
}

QUrl Auth::adminConsentUrl(const QString &tenant, const QString &clientId)
{
    QUrl url(QStringLiteral("https://login.microsoftonline.com/%1/adminconsent").arg(tenant.isEmpty() ? QStringLiteral("organizations") : tenant));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), clientId);
    url.setQuery(query);
    return url;
}

bool Auth::needsAdminConsent(const QString &error)
{
    // AADSTS65001: not consented; 90094 / 90099: admin approval required; 700016: the app does not
    // exist in that directory (never approved there, or the tenant is not on the allowed list);
    // consent_required comes back from the browser sign-in
    static const QRegularExpression codes(QStringLiteral("AADSTS(65001|90094|90099|700016)\\b|consent_required"));
    return codes.match(error).hasMatch();
}

bool Auth::isGuid(const QString &text)
{
    static const QRegularExpression guid(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return guid.match(text).hasMatch();
}

AuthConfig Auth::parseSetupText(const QString &text)
{
    static const QRegularExpression client(QStringLiteral("ClientId\\s*[=:]\\s*([0-9a-fA-F-]{36})"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tenant(QStringLiteral("TenantId\\s*[=:]\\s*([^\\s;,]+)"), QRegularExpression::CaseInsensitiveOption);
    return {client.match(text).captured(1), tenant.match(text).captured(1), {}};
}

QString Auth::resolveTenant(const QString &input, bool *offline)
{
    *offline = false;
    QString value = input.trimmed();
    if (isGuid(value)) {
        return value.toLower();
    }
    // A work e-mail address stands for its domain
    value = value.section(QLatin1Char('@'), -1).toLower();
    static const QRegularExpression domain(QStringLiteral("^[a-z0-9-]+(\\.[a-z0-9-]+)+$"));
    if (!domain.match(value).hasMatch()) {
        return {};
    }

    QNetworkAccessManager nam;
    QNetworkRequest request(QUrl(QStringLiteral("https://login.microsoftonline.com/%1/v2.0/.well-known/openid-configuration").arg(value)));
    request.setTransferTimeout(30 * 1000);
    QNetworkReply *reply = nam.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0) {
        *offline = true;
        return {};
    }
    // "issuer": "https://login.microsoftonline.com/<tenant id>/v2.0"
    const QString issuer = QJsonDocument::fromJson(reply->readAll()).object().value(QLatin1String("issuer")).toString();
    const QString tenantId = QUrl(issuer).path().section(QLatin1Char('/'), 1, 1);
    return isGuid(tenantId) ? tenantId.toLower() : QString();
}

void Auth::logout()
{
    TokenLock lock(this, TokenLock::NoWait);
    if (!lock.locked()) {
        // In use: the holder signs out when it is done (never waiting here, see TokenLock)
        QMutexLocker configLocker(&m_configMutex);
        m_pendingLogout = true;
        return;
    }
    logoutLocked();
}

void Auth::logoutLocked()
{
    m_accessToken.clear();
    setRefreshToken({});
    if (m_config.objectId.isEmpty() || !isConfigured()) {
        // Nothing of a user to remove (and the registration's old key is not ours to delete)
        return;
    }
    QStringList keys{keychainKey()};
    if (Config::legacyTokenUser() == m_config.objectId) {
        // This account's token from before 0.8.0, never moved
        keys.append(legacyKey());
        Config::setLegacyTokenUser({});
    }
    for (const QString &key : std::as_const(keys)) {
        keychainDelete(keychainService, key);
    }
    // Signed out: the app shows the setup again for this account. The account stays in
    // graphaccountsrc (other apps of the suite may use it), and its locations keep their account
    const AccountInfo current = Config::account();
    if (current.tenantId == m_config.tenantId && current.objectId == m_config.objectId) {
        Config::setCurrentAccountId({});
    }
}

QString Auth::readRefreshToken(QString *error)
{
    // Every stored sign-in belongs to a user; without one this would read the registration's key
    // from before 0.8.0, which only its own user may take over (below)
    if (m_config.objectId.isEmpty() || !isConfigured()) {
        if (error) {
            *error = QStringLiteral("not signed in");
        }
        return {};
    }
    QString token;
    QKeychain::Error code = QKeychain::NoError;
    if (keychainRead(keychainService, keychainKey(), &token, error, &code)) {
        m_keychainUnavailable = false;
        return token;
    }
    // Not "no such entry" but no answer at all (the wallet not open yet at login, access refused):
    // the sign-in is unknown, not gone
    m_keychainUnavailable = code != QKeychain::EntryNotFound;
    if (m_keychainUnavailable) {
        return {};
    }
    // Stored before 0.8.0 under the registration's key: move it to the account's own entry. Only
    // for the user it belongs to: another user of the same tenant and registration must not get it
    if (m_config.objectId.isEmpty() || Config::legacyTokenUser() != m_config.objectId) {
        return {};
    }
    QString legacyError;
    if (!moveKeychainEntry(keychainService, legacyKey(), keychainService, keychainKey(), &legacyError)) {
        return {};
    }
    Config::setLegacyTokenUser({});
    qCInfo(GRAPHSYNC) << "Moved the sign-in to the account's own KWallet entry";
    if (error) {
        error->clear();
    }
    return readRefreshToken(error);
}

bool Auth::writeRefreshToken(const QString &token, QString *error)
{
    return writeRefreshTokenFor(m_config, token, error);
}

bool Auth::writeRefreshTokenFor(const AuthConfig &config, const QString &token, QString *error)
{
    // Only under a user's own key, with parts that cannot hold the key's separator
    if (!isGuid(config.objectId) || !isGuid(config.clientId) || config.tenantId.isEmpty() || config.tenantId.contains(QLatin1Char(':'))) {
        *error = QStringLiteral("no signed-in user to store the sign-in for");
        return false;
    }
    return keychainWrite(keychainService, keychainKeyFor(config), token, error);
}

}
