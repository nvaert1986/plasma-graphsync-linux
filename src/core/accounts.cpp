// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copy of graphcore/accounts.cpp from plasma-graphmail-linux (same suite, same file format), until both
// apps link the shared graphcore library (phase 10). Keep it identical: change it there first.

#include "accounts.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QUuid>

namespace GraphCore
{

namespace
{
constexpr QLatin1StringView groupPrefix("Account ");

QString &application()
{
    static QString app;
    return app;
}

QString appClientKey()
{
    return QStringLiteral("ClientId-") + application();
}

KSharedConfig::Ptr config()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(Accounts::configFileName(), KConfig::NoGlobals);
    // Several processes write this file (mail client, sync client); always read the current state
    config->reparseConfiguration();
    return config;
}

Account readAccount(const KConfigGroup &group, const QString &id)
{
    Account account;
    account.id = id;
    account.tenantId = group.readEntry("TenantId");
    account.objectId = group.readEntry("ObjectId");
    account.userPrincipalName = group.readEntry("UserPrincipalName");
    account.displayName = group.readEntry("DisplayName");
    account.organization = group.readEntry("Organization");
    // This app's own registration, else the one shared by the apps (written before apps kept
    // their own)
    account.clientId = !application().isEmpty() && group.hasKey(appClientKey()) ? group.readEntry(appClientKey()) : group.readEntry("ClientId");
    return account;
}

void writeAccount(KConfigGroup group, const Account &account)
{
    // Not Accounts::defaultClientId(): that reparses the file and would drop the writes below
    const QString suiteDefault = group.config()->group(QStringLiteral("Registration")).readEntry("ClientId");
    group.writeEntry("TenantId", account.tenantId);
    group.writeEntry("ObjectId", account.objectId);
    group.writeEntry("UserPrincipalName", account.userPrincipalName);
    group.writeEntry("DisplayName", account.displayName);
    group.writeEntry("Organization", account.organization);
    if (application().isEmpty()) {
        if (account.clientId.isEmpty()) {
            group.deleteEntry("ClientId");
        } else {
            group.writeEntry("ClientId", account.clientId);
        }
        return;
    }
    // Only this app's key; the shared one belongs to the other apps. On the default registration
    // while the shared key names another one: say so, else this app would read that one
    QString own = account.clientId;
    if (own.isEmpty() && !group.readEntry("ClientId").isEmpty()) {
        own = suiteDefault;
    }
    if (own.isEmpty()) {
        group.deleteEntry(appClientKey());
    } else {
        group.writeEntry(appClientKey(), own);
    }
}
}

QString Account::effectiveClientId() const
{
    return clientId.isEmpty() ? Accounts::defaultClientId() : clientId;
}

QString Account::keychainKey() const
{
    return tenantId + QLatin1Char(':') + effectiveClientId() + QLatin1Char(':') + objectId;
}

namespace Accounts
{

QString configFileName()
{
    return QStringLiteral("graphaccountsrc");
}

void setApplication(const QString &app)
{
    application() = app;
}

QString defaultClientId()
{
    return config()->group(QStringLiteral("Registration")).readEntry("ClientId");
}

void setDefaultClientId(const QString &clientId)
{
    KSharedConfig::Ptr c = config();
    c->group(QStringLiteral("Registration")).writeEntry("ClientId", clientId);
    c->sync();
}

QList<Account> all()
{
    KSharedConfig::Ptr c = config();
    QList<Account> accounts;
    const QStringList groups = c->groupList();
    for (const QString &name : groups) {
        if (name.startsWith(groupPrefix)) {
            accounts.append(readAccount(c->group(name), name.mid(groupPrefix.size())));
        }
    }
    return accounts;
}

Account find(const QString &id)
{
    KSharedConfig::Ptr c = config();
    const QString name = groupPrefix + id;
    if (id.isEmpty() || !c->hasGroup(name)) {
        return {};
    }
    return readAccount(c->group(name), id);
}

Account findUser(const QString &tenantId, const QString &objectId)
{
    const QList<Account> accounts = all();
    for (const Account &account : accounts) {
        if (account.tenantId.compare(tenantId, Qt::CaseInsensitive) == 0 && account.objectId.compare(objectId, Qt::CaseInsensitive) == 0) {
            return account;
        }
    }
    return {};
}

Account add(Account account)
{
    const Account existing = findUser(account.tenantId, account.objectId);
    if (!existing.id.isEmpty()) {
        account.id = existing.id;
    } else if (account.id.isEmpty()) {
        KSharedConfig::Ptr c = config();
        do {
            account.id = QUuid::createUuid().toString(QUuid::Id128).left(8);
        } while (c->hasGroup(groupPrefix + account.id));
    }
    KSharedConfig::Ptr c = config();
    writeAccount(c->group(groupPrefix + account.id), account);
    c->sync();
    return account;
}

bool remove(const QString &id)
{
    KSharedConfig::Ptr c = config();
    const QString name = groupPrefix + id;
    if (id.isEmpty() || !c->hasGroup(name)) {
        return false;
    }
    c->deleteGroup(name);
    c->sync();
    return true;
}

}

}
