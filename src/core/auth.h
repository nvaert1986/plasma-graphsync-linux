// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config.h"

#include <QDateTime>
#include <QMutex>

#include <atomic>
#include <functional>
#include <QString>

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
    bool interactiveLogin(QString *error);
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

    // The tenant ID for what the user typed: a tenant ID, a domain or a work e-mail address.
    // Domains are looked up in the identity platform's public OpenID document (no sign-in
    // needed). Blocking. Empty when not found; *offline tells a missing connection apart
    static QString resolveTenant(const QString &input, bool *offline);
    // ClientId and TenantId from a setup text handed out by IT ("ClientId=... TenantId=...", in
    // any layout, like the [Auth] group of graphsyncrc); fields not found stay empty
    static AuthConfig parseSetupText(const QString &text);
    static bool isGuid(const QString &text);
    // The KWallet entry name of a registration's refresh token
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

private:
    QString authority() const;
    QString keychainKey() const;
    bool refresh(QString *error);

    QString readRefreshToken(QString *error);
    bool writeRefreshToken(const QString &token, QString *error);
    void storeAccountFromIdToken(const QString &idToken);

    AuthConfig m_config;
    QMutex m_mutex;
    QString m_accessToken;
    QDateTime m_expiresAt;
    QString m_refreshToken;
};

}
