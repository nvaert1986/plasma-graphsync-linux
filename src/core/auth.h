// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config.h"

#include <QDateTime>
#include <QMutex>

#include <atomic>
#include <functional>
#include <optional>
#include <QString>
#include <QUrl>

namespace GraphSync
{

// Entra ID sign-in for a single-tenant public client (authorization code + PKCE).
// The refresh token lives in the system keychain (KWallet / Secret Service); access tokens
// only in memory. accessToken() is thread-safe and may be called from any sync worker.
class Auth
{
public:
    explicit Auth(const AuthConfig &config);

    bool isConfigured() const;

    // Opens the browser and blocks (with a local event loop) until the redirect arrives.
    // Must run in a thread with an event loop, typically the main thread of graphsyncctl.
    // loginHint (a work e-mail address) pre-fills Microsoft's account picker.
    bool interactiveLogin(QString *error, const QString &loginHint = {});
    // Whether this build can sign in through the browser. It needs Qt 6.9 (scope tokens, the
    // "localhost" callback host, the ID token); older systems (Debian 13: Qt 6.8) sign in with
    // a code only, the same flow as "Sign In with a Code Instead".
    static constexpr bool browserLoginAvailable()
    {
        return QT_VERSION >= QT_VERSION_CHECK(6, 9, 0);
    }

    // Sign-in without the browser coming back to us: Microsoft hands out a short code that the
    // user enters at the shown address, on any device. show() gets the code and the address as
    // soon as they are known; then this blocks until the user is done, the code expires or
    // cancel is set. Needs "Allow public client flows" on the app registration.
    bool deviceCodeLogin(const std::function<void(const QString &code, const QString &address)> &show, const std::atomic_bool *cancel, QString *error);

    // Returns a valid access token, refreshing it when it expires within five minutes.
    // forceRefresh is used after a 401 from Graph.
    QString accessToken(QString *error, bool forceRefresh = false);

    bool hasStoredRefreshToken();
    void logout();

    // Switches to another app registration (Settings, setup wizard); tokens in memory are dropped
    void setConfig(const AuthConfig &config);
    // Registration, tenant and user; after a sign-in the tenant and user of who signed in
    AuthConfig config() const;

    // The tenant ID for what the user typed: a tenant ID, a domain or a work e-mail address.
    // Domains are looked up in the identity platform's public OpenID document (no sign-in
    // needed). Blocking. Empty when not found; *offline tells a missing connection apart
    static QString resolveTenant(const QString &input, bool *offline);
    // ClientId and TenantId from a setup text handed out by IT ("ClientId=... TenantId=...", in
    // any layout, like the [Auth] group of graphsyncrc); fields not found stay empty
    static AuthConfig parseSetupText(const QString &text);
    static bool isGuid(const QString &text);
    // The KWallet entry name of an account's refresh token: tenant:client:user, or tenant:client
    // before the first sign-in (and for tokens stored before 0.8.0, moved on first use)
    static QString keychainKeyFor(const AuthConfig &config);
    // Moves the refresh token stored by plasma-onedrive-linux (this client before 0.7.0) under the
    // legacy registration's key to this client's entry under target's key (Migration)
    static bool importLegacyToken(const AuthConfig &legacy, const AuthConfig &target, QString *error);
    // The same registration spelled differently (case, a domain resolved to the tenant ID): the
    // refresh token moves to the new key so the sign-in stays
    static bool moveToken(const AuthConfig &from, const AuthConfig &to, QString *error);
    // IDs as stored from now on: trimmed and lower case, so one registration has one KWallet key
    static AuthConfig normalized(const AuthConfig &config);

    static QString scopes();

    // Tests only: token requests go to this loopback server (http://127.0.0.1:<port>) instead of
    // Microsoft; anything else is ignored
    static void setLoginServerForTesting(const QUrl &server);
    // Tests only: a refresh token in memory, as after a sign-in
    void setRefreshTokenForTesting(const QString &token);

    // The page where an administrator of tenant approves the app registration for the whole
    // organisation (once per tenant; needed before its users can sign in to a multi-tenant app)
    static QUrl adminConsentUrl(const QString &tenant, const QString &clientId);
    // The sign-in failed because the organisation has not approved the app (yet)
    static bool needsAdminConsent(const QString &error);

private:
    QString authority() const;
    QString keychainKey() const;
    bool refresh(QString *error);

    QString readRefreshToken(QString *error);
    bool writeRefreshToken(const QString &token, QString *error);
    static bool writeRefreshTokenFor(const AuthConfig &config, const QString &token, QString *error);
    // Who signed in, from the ID token (empty when it names no user)
    static AccountInfo signedInUserFrom(const QString &idToken);
    // The end of every sign-in (m_mutex held): stores the refresh token under the new user's key,
    // then takes over that user and the tokens and registers the account
    bool completeSignIn(const QString &accessToken, const QDateTime &expiresAt, const QString &refreshToken, const QString &idToken, QString *error);
    void registerSignedInUser(const AccountInfo &user);
    static QString noUserError();
    // Where 0.7 kept this account's sign-in
    QString legacyKey() const;

    AuthConfig m_config;
    // m_mutex guards the tokens and is held across a refresh or a KWallet read (which run a local
    // event loop); it is only taken through TokenLock. m_configMutex guards m_config alone, so
    // config() never waits for a refresh: a request re-entered from that event loop asks for it
    // (throttle key). Writers of m_config hold both.
    class TokenLock;
    friend class TokenLock;
    mutable QMutex m_mutex;
    mutable QMutex m_configMutex;
    std::atomic<Qt::HANDLE> m_tokenOwner{nullptr};
    // Asked for while the tokens were in use (by another thread, or re-entered from this thread's
    // local event loop): done by the holder as it lets go. Guarded by m_configMutex
    std::optional<AuthConfig> m_pendingConfig;
    bool m_pendingLogout = false;
    // Whether a refresh token is known, for hasStoredRefreshToken() while the tokens are in use
    std::atomic_bool m_haveRefreshToken{false};
    void applyPendingLocked();
    void logoutLocked();
    void setRefreshToken(const QString &token);
    QString m_accessToken;
    QDateTime m_expiresAt;
    QString m_refreshToken;
};

}
