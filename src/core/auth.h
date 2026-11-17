// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config.h"

#include <QMutex>
#include <QString>
#include <QUrl>

#include <atomic>
#include <functional>
#include <memory>

namespace GraphCore
{
class ServiceTokenProvider;
}

namespace GraphSync
{

// One account's sign-in, as Graph Sync sees it. Since phase 10 it holds no tokens: the suite's
// account service (graphaccountsd, plasma-graphcore-linux) owns the one sign-in of every app and
// hands out access tokens over D-Bus. This class keeps the interface the sync engine, the daemon
// and the setup wizard used before, so they did not change. accessToken() is thread-safe.
class Auth
{
public:
    explicit Auth(const AuthConfig &config);
    ~Auth();

    bool isConfigured() const;

    // Signs in through the account service (the browser opens there) and blocks with a local
    // event loop until it is done. loginHint (a work e-mail address) pre-fills the account picker.
    bool interactiveLogin(QString *error, const QString &loginHint = {});
    // Whether this build can sign in through the browser (Qt 6.9); older systems use a code
    static constexpr bool browserLoginAvailable()
    {
        return QT_VERSION >= QT_VERSION_CHECK(6, 9, 0);
    }
    // Sign-in with a code entered on any device: show() gets the code and the address; blocks until
    // the user is done, the code expires or cancel is set
    bool deviceCodeLogin(const std::function<void(const QString &code, const QString &address)> &show, const std::atomic_bool *cancel, QString *error);

    // A valid access token from the account service; forceRefresh after a 401 from Graph
    QString accessToken(QString *error, bool forceRefresh = false);

    // The account service could not read the keychain (the wallet not open yet at login), or is
    // not reachable: whether this account is signed in is unknown, try again later
    bool keychainUnavailable() const;
    // Signed in (the account service has a sign-in for this account)
    bool hasStoredRefreshToken();
    // Signs the account out: for every app of the suite (one sign-in for all)
    void logout();

    // Switches to another app registration or user (Settings, setup wizard)
    void setConfig(const AuthConfig &config);
    // Registration, tenant and user; after a sign-in the tenant and user of who signed in
    AuthConfig config() const;

    // The tenant ID for what the user typed: a tenant ID, a domain or a work e-mail address.
    // Blocking. Empty when not found; *offline tells a missing connection apart
    static QString resolveTenant(const QString &input, bool *offline);
    // ClientId and TenantId from a setup text handed out by IT ("ClientId=... TenantId=...")
    static AuthConfig parseSetupText(const QString &text);
    static bool isGuid(const QString &text);
    // The keychain entry name of an account's sign-in: tenant:client:user, or tenant:client before
    // the first sign-in (and for tokens stored before 0.8.0)
    static QString keychainKeyFor(const AuthConfig &config);
    // Moves the refresh token stored by plasma-onedrive-linux (this client before 0.7.0) to Graph
    // Sync's own entry (Migration); the account service takes it over from there
    static bool importLegacyToken(const AuthConfig &legacy, const AuthConfig &target, QString *error);
    // The same registration spelled differently: Graph Sync's own stored sign-in moves along, so
    // the account service still finds it
    static bool moveToken(const AuthConfig &from, const AuthConfig &to, QString *error);
    // IDs as stored from now on: trimmed and lower case
    static AuthConfig normalized(const AuthConfig &config);

    // The page where an administrator approves the app registration for the organisation
    static QUrl adminConsentUrl(const QString &tenant, const QString &clientId);
    // The sign-in failed because the organisation has not approved the app (yet)
    static bool needsAdminConsent(const QString &error);

private:
    // The account service's id of this account (graphaccountsrc); empty before a sign-in
    QString accountId() const;
    bool signIn(bool deviceCode, const QString &loginHint, const std::function<void(const QString &, const QString &)> &show, const std::atomic_bool *cancel,
                QString *error);
    // Graph Sync's own sign-in from before 0.8.0 (under the registration's key): moved to the
    // account's key once, where the account service takes it over
    void moveLegacyToken();
    std::shared_ptr<GraphCore::ServiceTokenProvider> tokens();
    void noteProblem(const QString &problem);

    mutable QMutex m_mutex;
    AuthConfig m_config;
    std::shared_ptr<GraphCore::ServiceTokenProvider> m_tokens;
    QString m_tokensFor;
    // Raised by setConfig: a lookup that started before does not store its result
    quint64 m_generation = 0;
    // The service's last answer: a problem name, "Unreachable" (no answer at all) or empty
    QString m_lastProblem;
    std::atomic_bool m_legacyChecked = false;
};

}
