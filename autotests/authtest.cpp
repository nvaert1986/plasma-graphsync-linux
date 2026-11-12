// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "auth.h"

#include <QTest>

using namespace GraphSync;

class AuthTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void isGuid()
    {
        QVERIFY(Auth::isGuid(QStringLiteral("0a1b2c3d-4e5f-6789-abcd-ef0123456789")));
        QVERIFY(Auth::isGuid(QStringLiteral("0A1B2C3D-4E5F-6789-ABCD-EF0123456789")));
        QVERIFY(!Auth::isGuid(QStringLiteral("0a1b2c3d-4e5f-6789-abcd-ef012345678")));
        QVERIFY(!Auth::isGuid(QStringLiteral(" 0a1b2c3d-4e5f-6789-abcd-ef0123456789")));
        QVERIFY(!Auth::isGuid(QStringLiteral("contoso.onmicrosoft.com")));
    }

    void adminConsent()
    {
        const QString tenant = QStringLiteral("11111111-2222-3333-4444-555555555555");
        const QString client = QStringLiteral("0a1b2c3d-4e5f-6789-abcd-ef0123456789");
        QCOMPARE(Auth::adminConsentUrl(tenant, client).toString(), QStringLiteral("https://login.microsoftonline.com/%1/adminconsent?client_id=%2").arg(tenant, client));
        QVERIFY(Auth::needsAdminConsent(QStringLiteral("AADSTS65001: The user or administrator has not consented to use the application")));
        QVERIFY(Auth::needsAdminConsent(QStringLiteral("AADSTS90094: Admin consent is required")));
        QVERIFY(Auth::needsAdminConsent(QStringLiteral("AADSTS700016: Application with identifier 'x' was not found in the directory 'y'")));
        QVERIFY(Auth::needsAdminConsent(QStringLiteral("consent_required: ...")));
        // Not assigned to the app, or a dead refresh token: no approval page
        QVERIFY(!Auth::needsAdminConsent(QStringLiteral("AADSTS50105: The signed in user is not assigned to a role for the application")));
        QVERIFY(!Auth::needsAdminConsent(QStringLiteral("AADSTS7000160: something else")));
        QVERIFY(!Auth::needsAdminConsent(QStringLiteral("sign-in expired: AADSTS700082: The refresh token has expired")));
    }

    void parseSetupText_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("clientId");
        QTest::addColumn<QString>("tenantId");
        const QString client = QStringLiteral("0a1b2c3d-4e5f-6789-abcd-ef0123456789");
        const QString tenant = QStringLiteral("11111111-2222-3333-4444-555555555555");
        QTest::newRow("graphsyncrc group") << QStringLiteral("[Auth]\nClientId=%1\nTenantId=%2\n").arg(client, tenant) << client << tenant;
        QTest::newRow("one line") << QStringLiteral("ClientId=%1 TenantId=%2").arg(client, tenant) << client << tenant;
        QTest::newRow("newlines pasted as spaces, colons") << QStringLiteral("[Auth] clientid: %1 tenantid: contoso.com").arg(client)
                                                         << client << QStringLiteral("contoso.com");
        QTest::newRow("separated by semicolons") << QStringLiteral("ClientId=%1;TenantId=%2;").arg(client, tenant) << client << tenant;
        QTest::newRow("client only") << QStringLiteral("ClientId = %1").arg(client) << client << QString();
        QTest::newRow("bare id is not setup text") << client << QString() << QString();
    }
    void parseSetupText()
    {
        QFETCH(QString, text);
        QFETCH(QString, clientId);
        QFETCH(QString, tenantId);
        const AuthConfig config = Auth::parseSetupText(text);
        QCOMPARE(config.clientId, clientId);
        QCOMPARE(config.tenantId, tenantId);
    }

    void resolveTenantWithoutNetwork()
    {
        bool offline = true;
        // A tenant ID needs no lookup; rubbish is refused before any request
        QCOMPARE(Auth::resolveTenant(QStringLiteral(" 11111111-2222-3333-4444-55555555555A "), &offline),
                 QStringLiteral("11111111-2222-3333-4444-55555555555a"));
        QVERIFY(!offline);
        QCOMPARE(Auth::resolveTenant(QStringLiteral("not a domain"), &offline), QString());
        QCOMPARE(Auth::resolveTenant(QStringLiteral("someone@"), &offline), QString());
        QVERIFY(!offline);
    }
};

QTEST_GUILESS_MAIN(AuthTest)

#include "authtest.moc"
