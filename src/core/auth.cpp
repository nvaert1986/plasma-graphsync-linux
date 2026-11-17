// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "auth.h"
#include "accounts.h"
#include "graphsync_debug.h"
#include "serviceclient.h"
#include "signin.h"

#include <qt6keychain/keychain.h>

#include <QEventLoop>
#include <QThread>
#include <QTimer>

namespace GraphSync
{

namespace
{
// Graph Sync's own keychain service: only for the sign-ins stored before phase 10, which the
// account service takes over from here
const QString keychainService = QStringLiteral("plasma-graphsync-linux");

// QtKeychain jobs run on one thread of their own, one after the other (the secret service backend
// answers on the thread that first used it)
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

void onKeychainThread(const std::function<void()> &work)
{
    QObject *context = keychainContext();
    if (QThread::currentThread() == context->thread()) {
        work();
        return;
    }
    QMetaObject::invokeMethod(context, work, Qt::BlockingQueuedConnection);
}

template<typename Job>
bool runKeychainJob(Job *job, QString *error)
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
    return ok;
}

// Moves a refresh token between two keychain entries; the old one goes once the new one is written
bool moveKeychainEntry(const QString &fromService, const QString &fromKey, const QString &toService, const QString &toKey, QString *error)
{
    bool ok = false;
    QString failure;
    onKeychainThread([&] {
        QKeychain::ReadPasswordJob read(fromService);
        read.setKey(fromKey);
        if (!runKeychainJob(&read, &failure)) {
            return;
        }
        QKeychain::WritePasswordJob write(toService);
        write.setKey(toKey);
        write.setTextData(read.textData());
        if (!runKeychainJob(&write, &failure)) {
            return;
        }
        ok = true;
        if (fromService != toService || fromKey != toKey) {
            QKeychain::DeletePasswordJob remove(fromService);
            remove.setKey(fromKey);
            QString ignored;
            runKeychainJob(&remove, &ignored);
        }
    });
    if (!ok && error) {
        *error = failure;
    }
    return ok;
}
}

Auth::Auth(const AuthConfig &config)
    : m_config(config)
{
}

Auth::~Auth() = default;

bool Auth::isConfigured() const
{
    const AuthConfig config = this->config();
    // The parts end up in a keychain key "tenant:client:user": none may hold the separator
    return isGuid(config.clientId) && !config.tenantId.isEmpty() && !config.tenantId.contains(QLatin1Char(':'))
        && (config.objectId.isEmpty() || isGuid(config.objectId));
}

AuthConfig Auth::config() const
{
    QMutexLocker locker(&m_mutex);
    return m_config;
}

void Auth::setConfig(const AuthConfig &config)
{
    QMutexLocker locker(&m_mutex);
    m_config = config;
    ++m_generation;
    m_tokens.reset();
    m_tokensFor.clear();
    m_lastProblem.clear();
    m_legacyChecked = false;
}

QString Auth::accountId() const
{
    const AuthConfig config = this->config();
    if (config.objectId.isEmpty() || config.tenantId.isEmpty()) {
        return {};
    }
    return GraphCore::Accounts::findUser(config.tenantId, config.objectId).id;
}

std::shared_ptr<GraphCore::ServiceTokenProvider> Auth::tokens()
{
    quint64 generation = 0;
    {
        // Every Graph request of the sync workers asks: the account is looked up in
        // graphaccountsrc only once per configuration (setConfig starts over)
        QMutexLocker locker(&m_mutex);
        if (m_tokens) {
            return m_tokens;
        }
        generation = m_generation;
    }
    const QString id = accountId();
    if (id.isEmpty()) {
        return nullptr;
    }
    QMutexLocker locker(&m_mutex);
    if (generation != m_generation) {
        // The configuration changed meanwhile: this lookup was for the old one
        return std::make_shared<GraphCore::ServiceTokenProvider>(id);
    }
    if (!m_tokens) {
        m_tokens = std::make_shared<GraphCore::ServiceTokenProvider>(id);
        m_tokensFor = id;
    }
    return m_tokens;
}

void Auth::noteProblem(const QString &problem)
{
    QMutexLocker locker(&m_mutex);
    m_lastProblem = problem;
}

void Auth::moveLegacyToken()
{
    if (m_legacyChecked.exchange(true)) {
        return;
    }
    const AuthConfig config = this->config();
    // Only for the user it belongs to: another user of the same tenant and registration must not get it
    if (config.objectId.isEmpty() || !isConfigured() || Config::legacyTokenUser() != config.objectId) {
        return;
    }
    const QString recorded = Config::legacyTokenKey();
    const QString legacyKey = recorded.isEmpty() ? keychainKeyFor({config.clientId, config.tenantId, {}}) : recorded;
    QString error;
    if (moveKeychainEntry(keychainService, legacyKey, keychainService, keychainKeyFor(config), &error)) {
        Config::setLegacyTokenUser({});
        qCInfo(GRAPHSYNC) << "Moved the sign-in to the account's own KWallet entry";
    } else {
        // Tried again next time (the wallet may not be open yet)
        m_legacyChecked = false;
    }
}

QString Auth::accessToken(QString *error, bool forceRefresh)
{
    const std::shared_ptr<GraphCore::ServiceTokenProvider> provider = tokens();
    if (!provider) {
        if (error) {
            *error = QStringLiteral("not signed in (graphsyncctl login)");
        }
        noteProblem(QStringLiteral("NotSignedIn"));
        return {};
    }
    moveLegacyToken();
    QString failure;
    const QString token = provider->accessToken(&failure, forceRefresh);
    if (token.isEmpty()) {
        const QString problem = provider->lastProblem();
        noteProblem(problem.isEmpty() ? QStringLiteral("Unreachable") : problem);
        if (error) {
            *error = problem == QLatin1String("NotSignedIn") ? QStringLiteral("not signed in (graphsyncctl login)") : failure;
        }
        return {};
    }
    noteProblem({});
    return token;
}

bool Auth::keychainUnavailable() const
{
    QMutexLocker locker(&m_mutex);
    return m_lastProblem == QLatin1String("KeychainLocked") || m_lastProblem == QLatin1String("Unreachable");
}

bool Auth::hasStoredRefreshToken()
{
    const QString id = accountId();
    if (id.isEmpty() || !isConfigured()) {
        return false;
    }
    // What the service already knows, without a token request
    const QList<GraphCore::ServiceAccount> accounts = GraphCore::AccountsClient().accounts();
    for (const GraphCore::ServiceAccount &account : accounts) {
        if (account.id != id) {
            continue;
        }
        if (account.state == QLatin1String("signedIn")) {
            return true;
        }
        if (account.state == QLatin1String("signInNeeded") || account.state == QLatin1String("signedOut")) {
            return false;
        }
    }
    // Not asked yet (this also takes over a sign-in stored before phase 10)
    QString error;
    if (!accessToken(&error).isEmpty()) {
        return true;
    }
    QMutexLocker locker(&m_mutex);
    // Signed in, only the connection or Microsoft is in the way
    return m_lastProblem == QLatin1String("Offline") || m_lastProblem == QLatin1String("Other");
}

void Auth::logout()
{
    const QString id = accountId();
    if (!id.isEmpty()) {
        QString error;
        if (!GraphCore::AccountsClient().signOut(id, &error)) {
            qCWarning(GRAPHSYNC).noquote() << "Could not sign out:" << error;
        }
    }
    {
        QMutexLocker locker(&m_mutex);
        m_tokens.reset();
        m_tokensFor.clear();
    }
    // Signed out: the app shows the setup again for this account. The account stays in
    // graphaccountsrc (other apps of the suite may use it), and its locations keep their account
    const AuthConfig config = this->config();
    const AccountInfo current = Config::account();
    if (!config.objectId.isEmpty() && current.tenantId == config.tenantId && current.objectId == config.objectId) {
        Config::setCurrentAccountId({});
    }
}

bool Auth::interactiveLogin(QString *error, const QString &loginHint)
{
    return signIn(false, loginHint, {}, nullptr, error);
}

bool Auth::deviceCodeLogin(const std::function<void(const QString &, const QString &)> &show, const std::atomic_bool *cancel, QString *error)
{
    return signIn(true, {}, show, cancel, error);
}

bool Auth::signIn(bool deviceCode, const QString &loginHint, const std::function<void(const QString &, const QString &)> &show, const std::atomic_bool *cancel,
                  QString *error)
{
    const AuthConfig config = this->config();
    if (!isGuid(config.clientId) || config.tenantId.isEmpty() || config.tenantId.contains(QLatin1Char(':'))) {
        *error = QStringLiteral("client id or tenant id not configured (graphsyncctl configure)");
        return false;
    }
    if (!deviceCode && !browserLoginAvailable()) {
        *error = QStringLiteral("signing in through the browser needs Qt 6.9 or newer; sign in with a code instead (graphsyncctl login device)");
        return false;
    }
    GraphCore::AccountsClient accounts;
    QEventLoop loop;
    QString request;
    QString signedIn;
    QString failure;
    QObject::connect(&accounts, &GraphCore::AccountsClient::signInProgress, &loop, [&](const QString &id, const QString &code, const QString &address) {
        if (id == request && show) {
            show(code, address);
        }
    });
    QObject::connect(&accounts, &GraphCore::AccountsClient::signInFinished, &loop, [&](const QString &id, const QString &account, const QString &message) {
        if (id != request) {
            return;
        }
        signedIn = account;
        failure = message;
        loop.quit();
    });
    QTimer cancelCheck;
    if (cancel) {
        QObject::connect(&cancelCheck, &QTimer::timeout, &loop, [&] {
            if (cancel->load()) {
                accounts.cancelSignIn(request);
                failure = QStringLiteral("cancelled");
                loop.quit();
            }
        });
        cancelCheck.start(250);
    }
    request = accounts.signIn(config.clientId, config.tenantId, loginHint, deviceCode, &failure);
    if (request.isEmpty()) {
        *error = failure.isEmpty() ? QStringLiteral("the account service did not answer") : failure;
        return false;
    }
    loop.exec();
    if (signedIn.isEmpty()) {
        *error = failure.isEmpty() ? QStringLiteral("sign-in did not complete") : failure;
        return false;
    }
    // Who signed in: the service stored the account in graphaccountsrc
    AccountInfo user = GraphCore::Accounts::find(signedIn);
    if (!user.isValid()) {
        *error = QStringLiteral("the signed-in account was not found");
        return false;
    }
    setConfig({config.clientId, user.tenantId, user.objectId});
    user.clientId = config.clientId;
    // The organisation name comes from Graph later; a known account keeps its own
    Config::registerAccount(user);
    m_legacyChecked = true;
    return true;
}

QString Auth::resolveTenant(const QString &input, bool *offline)
{
    return GraphCore::SignIn::resolveTenant(input, offline);
}

AuthConfig Auth::parseSetupText(const QString &text)
{
    const GraphCore::SignIn::SetupText setup = GraphCore::SignIn::parseSetupText(text);
    return {setup.clientId, setup.tenant, {}};
}

bool Auth::isGuid(const QString &text)
{
    return GraphCore::SignIn::isGuid(text);
}

QString Auth::keychainKeyFor(const AuthConfig &config)
{
    const QString registration = config.tenantId + QLatin1Char(':') + config.clientId;
    return config.objectId.isEmpty() ? registration : registration + QLatin1Char(':') + config.objectId;
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
    return {GraphCore::SignIn::normalizedId(config.clientId), GraphCore::SignIn::normalizedId(config.tenantId), GraphCore::SignIn::normalizedId(config.objectId)};
}

QUrl Auth::adminConsentUrl(const QString &tenant, const QString &clientId)
{
    return GraphCore::SignIn::adminConsentUrl(tenant, clientId);
}

bool Auth::needsAdminConsent(const QString &error)
{
    return GraphCore::SignIn::needsAdminConsent(error);
}

}
