// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copy of graphcore/accounts.h from plasma-graphmail-linux (same suite, same file format), until both
// apps link the shared graphcore library (phase 10). Keep it identical: change it there first.

#pragma once

#include <QList>
#include <QString>

namespace GraphCore
{

// One signed-in work or school account, stored as an [Account <id>] group in graphaccountsrc. The
// file is shared by every app of the suite (mail client, sync client), so an account is added once.
struct Account {
    QString id; // local and stable, assigned by Accounts::add()
    QString tenantId;
    QString objectId; // the user's object id in Entra ID (the "oid" claim)
    QString userPrincipalName;
    QString displayName;
    // Tenant display name, from Graph /organization
    QString organization;
    // Empty: the suite's default app registration (Accounts::defaultClientId()). The app
    // registration of this app (Accounts::setApplication()): apps may sign the same user in with
    // different registrations, each keeps its own
    QString clientId;

    bool isValid() const
    {
        return !id.isEmpty() && !tenantId.isEmpty() && !objectId.isEmpty();
    }
    // The app registration this account signs in with
    QString effectiveClientId() const;
    // Key of the account's refresh token in the system keychain: tenant:client:user
    QString keychainKey() const;
};

namespace Accounts
{
QString configFileName();

// The app this process belongs to (its keychain service, e.g. "plasma-graphmail-linux"), set once
// at start. An account's client ID is then read from and written to that app's own key
// (ClientId-<app>), so one app signing in with another registration does not change the other
// apps' (whose KWallet entries are keyed by it). Without it: the shared ClientId, as before.
void setApplication(const QString &app);

// The app registration (client ID) used by accounts without their own. [Registration] ClientId,
// which a package or IT may also ship in /etc/xdg/graphaccountsrc.
QString defaultClientId();
void setDefaultClientId(const QString &clientId);

QList<Account> all();
Account find(const QString &id);
// The account of this user in this tenant, when it was added before
Account findUser(const QString &tenantId, const QString &objectId);
// Adds the account, or updates the one of the same user in the same tenant (keeping its id).
// Returns the stored account.
Account add(Account account);
bool remove(const QString &id);
}

}
