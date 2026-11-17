// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "updatecheck.h"

#include <KConfig>
#include <KConfigGroup>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTest>

using namespace GraphSync;

namespace
{
void setLatest(const QString &version)
{
    KConfig config(QDir(Config::stateDirectory()).filePath(QStringLiteral("updatecheck")), KConfig::SimpleConfig);
    config.group(QStringLiteral("Update")).writeEntry("Latest", version);
    config.sync();
}

void setAnnouncedAt(const QDateTime &when)
{
    KConfig config(QDir(Config::stateDirectory()).filePath(QStringLiteral("updatecheck")), KConfig::SimpleConfig);
    config.group(QStringLiteral("Update")).writeEntry("AnnouncedAt", when.toString(Qt::ISODate));
    config.sync();
}

// GitHub's answer to releases/latest, with the fields that matter
QByteArray answer(const QJsonObject &release)
{
    return QJsonDocument(release).toJson();
}
}

class UpdateCheckTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QFile::remove(QDir(Config::stateDirectory()).filePath(QStringLiteral("updatecheck")));
    }

    void parseLatest()
    {
        const auto release = UpdateCheck::parseLatest(answer({{QStringLiteral("tag_name"), QStringLiteral("v0.9.5")},
                                                              {QStringLiteral("draft"), false},
                                                              {QStringLiteral("prerelease"), false},
                                                              {QStringLiteral("html_url"), QStringLiteral("https://example.com/elsewhere")}}));
        QVERIFY(release);
        QCOMPARE(release->version, QStringLiteral("0.9.5"));
        // The page is made here, never taken from the answer
        QCOMPARE(release->page, QUrl(QStringLiteral("https://github.com/nvaert1986/plasma-graphsync-linux/releases/tag/v0.9.5")));
        QCOMPARE(UpdateCheck::parseLatest(answer({{QStringLiteral("tag_name"), QStringLiteral("1.0")}}))->version, QStringLiteral("1.0"));

        QVERIFY(!UpdateCheck::parseLatest(answer({{QStringLiteral("tag_name"), QStringLiteral("v1.0.0")}, {QStringLiteral("prerelease"), true}})));
        QVERIFY(!UpdateCheck::parseLatest(answer({{QStringLiteral("tag_name"), QStringLiteral("v1.0.0")}, {QStringLiteral("draft"), true}})));
        QVERIFY(!UpdateCheck::parseLatest(answer({{QStringLiteral("tag_name"), QStringLiteral("v1.0.0-beta1")}})));
        QVERIFY(!UpdateCheck::parseLatest(answer({{QStringLiteral("tag_name"), QStringLiteral("v1.0/../../x")}})));
        QVERIFY(!UpdateCheck::parseLatest(answer({{QStringLiteral("message"), QStringLiteral("API rate limit exceeded")}})));
        QVERIFY(!UpdateCheck::parseLatest("not json"));
    }

    void isNewer()
    {
        QVERIFY(UpdateCheck::isNewer(QStringLiteral("0.9.10"), QStringLiteral("0.9.4")));
        QVERIFY(UpdateCheck::isNewer(QStringLiteral("1.0"), QStringLiteral("0.9.4")));
        QVERIFY(UpdateCheck::isNewer(QStringLiteral("v0.9.4.1"), QStringLiteral("0.9.4")));
        QVERIFY(!UpdateCheck::isNewer(QStringLiteral("0.9.4"), QStringLiteral("0.9.4")));
        QVERIFY(!UpdateCheck::isNewer(QStringLiteral("0.9.3"), QStringLiteral("0.9.4")));
        QVERIFY(!UpdateCheck::isNewer(QString(), QStringLiteral("0.9.4")));
        QVERIFY(!UpdateCheck::isNewer(QStringLiteral("garbage"), QStringLiteral("0.9.4")));
    }

    void announceUntilSkipped()
    {
        UpdateChecker checker;
        QVERIFY(checker.available().isEmpty());
        QVERIFY(!checker.announce(true));

        // Older than or the same as this build: nothing to tell
        setLatest(UpdateCheck::currentVersion());
        QVERIFY(checker.available().isEmpty());
        setLatest(QStringLiteral("0.1"));
        QVERIFY(checker.available().isEmpty());

        setLatest(QStringLiteral("99.0"));
        QCOMPARE(checker.available(), QStringLiteral("99.0"));
        QCOMPARE(checker.page(), QUrl(QStringLiteral("https://github.com/nvaert1986/plasma-graphsync-linux/releases/tag/v99.0")));
        QVERIFY(checker.announce(false));
        checker.markAnnounced();
        // Not again the same day, but again at the next login and the next day
        QVERIFY(!checker.announce(false));
        QVERIFY(checker.announce(true));
        setAnnouncedAt(QDateTime::currentDateTimeUtc().addSecs(-24 * 3600));
        QVERIFY(checker.announce(false));
        // Still shown (the tray) until skipped
        QCOMPARE(checker.available(), QStringLiteral("99.0"));

        checker.skip(QStringLiteral("99.0"));
        QVERIFY(checker.available().isEmpty());
        QVERIFY(!checker.announce(false));
        QVERIFY(!checker.announce(true));

        // A later version after a skipped one is told about again
        setLatest(QStringLiteral("99.1"));
        QCOMPARE(checker.available(), QStringLiteral("99.1"));
        QVERIFY(checker.announce(false));
    }
};

QTEST_GUILESS_MAIN(UpdateCheckTest)

#include "updatechecktest.moc"
