// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "auth.h"
#include "config.h"
#include "migration.h"

#include <KConfig>
#include <KConfigGroup>

#include <QDir>
#include <QStandardPaths>
#include <QTest>

using namespace GraphSync;

namespace
{
const QString clientA = QStringLiteral("0a1b2c3d-4e5f-6789-abcd-ef0123456789");
const QString clientB = QStringLiteral("99999999-8888-7777-6666-555555555555");
const QString tenant = QStringLiteral("11111111-2222-3333-4444-555555555555");
const QString user = QStringLiteral("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");

QString configFile(const QString &name)
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath(name);
}

// graphsyncrc as 0.7.x wrote it: the account in [Account], two locations without an account
void writeOldSetup(const QString &objectId, const QString &clientId)
{
    KConfig file(configFile(QStringLiteral("graphsyncrc")), KConfig::SimpleConfig);
    file.group(QStringLiteral("Auth")).writeEntry("ClientId", clientId);
    file.group(QStringLiteral("Auth")).writeEntry("TenantId", tenant);
    KConfigGroup account = file.group(QStringLiteral("Account"));
    account.writeEntry("ObjectId", objectId);
    account.writeEntry("TenantId", objectId.isEmpty() ? QString() : tenant);
    account.writeEntry("UserPrincipalName", QStringLiteral("alex@contoso.com"));
    account.writeEntry("DisplayName", QStringLiteral("Alex Example"));
    account.writeEntry("Organization", QStringLiteral("Contoso"));
    file.group(QStringLiteral("General")).writeEntry("SetupDone", true);
    file.group(QStringLiteral("Root r1")).writeEntry("LocalPath", QStringLiteral("/tmp/r1"));
    file.group(QStringLiteral("Root r2")).writeEntry("LocalPath", QStringLiteral("/tmp/r2"));
    file.sync();
}
}

class AccountsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        GraphCore::Accounts::setApplication(QStringLiteral("plasma-graphsync-linux"));
    }

    void init()
    {
        QFile::remove(configFile(QStringLiteral("graphsyncrc")));
        QFile::remove(configFile(GraphCore::Accounts::configFileName()));
    }

    void keychainKey()
    {
        QCOMPARE(Auth::keychainKeyFor({clientA, tenant, {}}), tenant + QLatin1Char(':') + clientA);
        QCOMPARE(Auth::keychainKeyFor({clientA, tenant, user}), tenant + QLatin1Char(':') + clientA + QLatin1Char(':') + user);
    }

    void adoptMovesTheAccount()
    {
        writeOldSetup(user, clientA);
        Migration::adoptSharedAccounts();

        const QList<AccountInfo> accounts = GraphCore::Accounts::all();
        QCOMPARE(accounts.size(), 1);
        const AccountInfo account = accounts.constFirst();
        QCOMPARE(account.objectId, user);
        QCOMPARE(account.tenantId, tenant);
        QCOMPARE(account.userPrincipalName, QStringLiteral("alex@contoso.com"));
        QCOMPARE(account.organization, QStringLiteral("Contoso"));
        // The first registration becomes the suite default, so the account names none of its own
        QCOMPARE(GraphCore::Accounts::defaultClientId(), clientA);
        QVERIFY(account.clientId.isEmpty());

        QCOMPARE(Config::account().id, account.id);
        const QList<RootConfig> roots = Config::roots();
        QCOMPARE(roots.size(), 2);
        for (const RootConfig &root : roots) {
            QCOMPARE(root.accountId, account.id);
        }
        QVERIFY(!KConfig(configFile(QStringLiteral("graphsyncrc")), KConfig::SimpleConfig).hasGroup(QStringLiteral("Account")));
        QCOMPARE(Config::authFor(account).objectId, user);
        QCOMPARE(Config::currentAuth().clientId, clientA);

        // A second run changes nothing
        Migration::adoptSharedAccounts();
        QCOMPARE(GraphCore::Accounts::all().size(), 1);
        QCOMPARE(Config::account().id, account.id);
    }

    void adoptReusesTheSuiteAccount()
    {
        // Added by the mail client before: same user, same tenant, its own id
        GraphCore::Accounts::setDefaultClientId(clientA);
        GraphCore::Account mail;
        mail.tenantId = tenant;
        mail.objectId = user;
        mail.userPrincipalName = QStringLiteral("alex@contoso.com");
        mail.organization = QStringLiteral("Contoso Ltd");
        const GraphCore::Account stored = GraphCore::Accounts::add(mail);

        writeOldSetup(user, clientA);
        Migration::adoptSharedAccounts();
        QCOMPARE(GraphCore::Accounts::all().size(), 1);
        QCOMPARE(Config::account().id, stored.id);
        QCOMPARE(Config::roots().constFirst().accountId, stored.id);
    }

    void adoptKeepsAnotherRegistration()
    {
        // The suite uses another registration: this account keeps its own
        GraphCore::Accounts::setDefaultClientId(clientB);
        writeOldSetup(user, clientA);
        Migration::adoptSharedAccounts();
        const AccountInfo account = Config::account();
        QCOMPARE(account.clientId, clientA);
        QCOMPARE(account.effectiveClientId(), clientA);
        QCOMPARE(GraphCore::Accounts::defaultClientId(), clientB);
    }

    void adoptWhileSignedOut()
    {
        writeOldSetup(QString(), clientA);
        Migration::adoptSharedAccounts();
        QVERIFY(GraphCore::Accounts::all().isEmpty());
        QVERIFY(!Config::account().isValid());
        QVERIFY(Config::roots().constFirst().accountId.isEmpty());
        // Signing in later claims the locations
        AccountInfo signedIn;
        signedIn.tenantId = tenant;
        signedIn.objectId = user;
        signedIn.clientId = clientA;
        const AccountInfo stored = Config::registerAccount(signedIn);
        QCOMPARE(Config::roots().constFirst().accountId, stored.id);
        QCOMPARE(Config::currentAuth().objectId, user);
    }

    void accountForLink()
    {
        GraphCore::Accounts::setDefaultClientId(clientA);
        const auto add = [](const QString &id, const QString &tenantId, const QString &objectId, const QString &upn) {
            GraphCore::Account account;
            account.id = id;
            account.tenantId = tenantId;
            account.objectId = objectId;
            account.userPrincipalName = upn;
            return GraphCore::Accounts::add(account);
        };
        add(QStringLiteral("acc1"), tenant, user, QStringLiteral("alex@contoso.com"));
        add(QStringLiteral("acc2"), QStringLiteral("22222222-2222-3333-4444-555555555555"), QStringLiteral("bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee"),
            QStringLiteral("alex@fabrikam.com"));
        Config::setCurrentAccountId(QStringLiteral("acc1"));
        RootConfig library;
        library.id = QStringLiteral("r2");
        library.localPath = QStringLiteral("/tmp/fabrikam");
        library.webUrl = QStringLiteral("https://fabrikam-my.sharepoint.com/personal/alex/Documents");
        library.accountId = QStringLiteral("acc2");
        Config::addRoot(library);

        const QString fabrikam = QStringLiteral("fabrikam.sharepoint.com");
        // The user the link names, by object id (any case) or address
        QCOMPARE(Config::accountForLink(QStringLiteral("BBBBBBBB-bbbb-cccc-dddd-eeeeeeeeeeee"), {}, QStringLiteral("other.sharepoint.com")).id, QStringLiteral("acc2"));
        QCOMPARE(Config::accountForLink({}, QStringLiteral("Alex@Fabrikam.com"), QStringLiteral("other.sharepoint.com")).id, QStringLiteral("acc2"));
        // No user in the link: the account that syncs from that organisation (-my is the same one)
        QCOMPARE(Config::accountForLink({}, {}, fabrikam).id, QStringLiteral("acc2"));
        // Unknown organisation and user: the current account
        QCOMPARE(Config::accountForLink({}, QStringLiteral("sam@northwind.com"), QStringLiteral("northwind.sharepoint.com")).id, QStringLiteral("acc1"));
        // A location of that organisation whose account is gone does not decide it
        RootConfig orphan;
        orphan.id = QStringLiteral("r0");
        orphan.localPath = QStringLiteral("/tmp/fabrikam-old");
        orphan.webUrl = QStringLiteral("https://fabrikam.sharepoint.com/sites/old/Documents");
        orphan.accountId = QStringLiteral("gone");
        Config::addRoot(orphan);
        QCOMPARE(Config::accountForLink({}, {}, fabrikam).id, QStringLiteral("acc2"));
    }

    void organizationLabel()
    {
        GraphCore::Account first;
        first.id = QStringLiteral("acc1");
        first.tenantId = tenant;
        first.objectId = user;
        first.userPrincipalName = QStringLiteral("alex@contoso.com");
        first.organization = QStringLiteral("Contoso");
        GraphCore::Accounts::add(first);
        GraphCore::Account second = first;
        second.id = QStringLiteral("acc2");
        second.objectId = QStringLiteral("cccccccc-bbbb-cccc-dddd-eeeeeeeeeeee");
        second.userPrincipalName = QStringLiteral("sam@contoso.com");
        GraphCore::Accounts::add(second);
        // Nobody of Contoso syncs here yet: the plain name
        QCOMPARE(Config::organizationLabel(second), QStringLiteral("Contoso"));
        RootConfig root;
        root.id = QStringLiteral("r1");
        root.localPath = QStringLiteral("/tmp/contoso");
        root.accountId = QStringLiteral("acc1");
        Config::addRoot(root);
        // A second user of the same organisation gets the address, the first keeps the name
        QCOMPARE(Config::organizationLabel(second), QStringLiteral("Contoso (sam@contoso.com)"));
        QCOMPARE(Config::organizationLabel(first), QStringLiteral("Contoso"));
        // Also once the second one syncs: each keeps the name its folders have
        RootConfig samRoot;
        samRoot.id = QStringLiteral("r2");
        samRoot.localPath = QStringLiteral("/tmp/SharePoint - Contoso (sam@contoso.com)/Team");
        samRoot.accountId = QStringLiteral("acc2");
        Config::addRoot(samRoot);
        QCOMPARE(Config::organizationLabel(first), QStringLiteral("Contoso"));
        QCOMPARE(Config::organizationLabel(second), QStringLiteral("Contoso (sam@contoso.com)"));
    }

    void orphansStayWithTheirOrganisation()
    {
        // Set up while signed out, by a 0.7 setup of tenant
        writeOldSetup(QString(), clientA);
        Migration::adoptSharedAccounts();
        AccountInfo foreign;
        foreign.tenantId = QStringLiteral("22222222-2222-3333-4444-555555555555");
        foreign.objectId = QStringLiteral("bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee");
        foreign.clientId = clientA;
        Config::registerAccount(foreign);
        // Another organisation does not get them
        QVERIFY(Config::roots().constFirst().accountId.isEmpty());
        AccountInfo own;
        own.tenantId = tenant;
        own.objectId = user;
        own.clientId = clientA;
        const AccountInfo stored = Config::registerAccount(own);
        QCOMPARE(Config::roots().constFirst().accountId, stored.id);
    }

    void rootFindsItsUserAgain()
    {
        GraphCore::Accounts::setDefaultClientId(clientA);
        AccountInfo account;
        account.tenantId = tenant;
        account.objectId = user;
        account.clientId = clientA;
        const AccountInfo first = Config::registerAccount(account);
        RootConfig root;
        root.id = QStringLiteral("r1");
        root.localPath = QStringLiteral("/tmp/r1");
        root.accountId = first.id;
        Config::addRoot(root);
        // Another app removes the shared entry; the same user signs in again under a new id
        GraphCore::Accounts::remove(first.id);
        const AccountInfo again = Config::registerAccount(account);
        QVERIFY(again.id != first.id);
        QCOMPARE(Config::roots().constFirst().accountId, again.id);
    }

    void noKeychainWithoutUser()
    {
        // Without a user it must not touch the registration's key from before 0.8.0
        Auth registrationOnly({clientA, tenant, {}});
        QVERIFY(!registrationOnly.hasStoredRefreshToken());
        // A separator in a value would let one key stand for another
        QVERIFY(!Auth({clientA, tenant + QStringLiteral(":x"), user}).isConfigured());
        QVERIFY(!Auth({QStringLiteral("not-a-guid"), tenant, user}).isConfigured());
        QVERIFY(!Auth({clientA, tenant, QStringLiteral("x:y")}).isConfigured());
        QVERIFY(Auth({clientA, tenant, user}).isConfigured());
    }

    void otherAppsRegistrationIsLeftAlone()
    {
        // Graph Mail signed this user in with another registration (shared key from before apps
        // kept their own, and its own key)
        GraphCore::Accounts::setDefaultClientId(clientA);
        KConfig shared(configFile(GraphCore::Accounts::configFileName()), KConfig::SimpleConfig);
        KConfigGroup entry = shared.group(QStringLiteral("Account mail1"));
        entry.writeEntry("TenantId", tenant);
        entry.writeEntry("ObjectId", user);
        entry.writeEntry("ClientId", clientB);
        entry.writeEntry("ClientId-plasma-graphmail-linux", clientB);
        shared.sync();

        // Graph Sync signs the same user in with the suite's default registration
        AccountInfo signedIn;
        signedIn.tenantId = tenant;
        signedIn.objectId = user;
        signedIn.clientId = clientA;
        const AccountInfo stored = Config::registerAccount(signedIn);
        QCOMPARE(stored.id, QStringLiteral("mail1"));
        QCOMPARE(Config::authFor(GraphCore::Accounts::find(stored.id)).clientId, clientA);
        // Graph Mail's registration is untouched
        const KConfig after(configFile(GraphCore::Accounts::configFileName()), KConfig::SimpleConfig);
        QCOMPARE(after.group(QStringLiteral("Account mail1")).readEntry("ClientId", QString()), clientB);
        QCOMPARE(after.group(QStringLiteral("Account mail1")).readEntry("ClientId-plasma-graphmail-linux", QString()), clientB);
    }

    void rootKeepsItsAccount()
    {
        RootConfig root;
        root.id = QStringLiteral("s1");
        root.localPath = QStringLiteral("/tmp/s1");
        root.accountId = QStringLiteral("acc1");
        Config::addRoot(root);
        // Shortcut roots are written again without an account after a rename
        root.accountId.clear();
        root.name = QStringLiteral("renamed");
        QCOMPARE(Config::addRoot(root).accountId, QStringLiteral("acc1"));
        QCOMPARE(Config::roots().constFirst().accountId, QStringLiteral("acc1"));
    }
};

QTEST_GUILESS_MAIN(AccountsTest)
#include "accountstest.moc"
