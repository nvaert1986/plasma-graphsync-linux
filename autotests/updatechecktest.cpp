// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Graph Sync's side of the update check: its repository, version and state file. The check itself
// (asking GitHub, versions, announce and skip) is the shared core's and tested there.

#include "config.h"
#include "updatecheck.h"

#include <KConfig>
#include <KConfigGroup>

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTest>

using namespace GraphSync;

class UpdateCheckTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QFile::remove(QDir(Config::stateDirectory()).filePath(QStringLiteral("updatecheck")));
    }

    void settings()
    {
        const GraphCore::UpdateCheckSettings settings = UpdateCheck::settings();
        QCOMPARE(settings.repository, QStringLiteral("nvaert1986/plasma-graphsync-linux"));
        QCOMPARE(settings.currentVersion, UpdateCheck::currentVersion());
        QVERIFY(settings.userAgent.startsWith(QLatin1String("plasma-graphsync-linux/")));
        // The same state file as before the check moved to the core
        QCOMPARE(settings.stateFile, QDir(Config::stateDirectory()).filePath(QStringLiteral("updatecheck")));
    }

    void stateFromBeforeIsKept()
    {
        // What 0.11 and older wrote: a newer release that was skipped stays skipped
        {
            KConfig config(QDir(Config::stateDirectory()).filePath(QStringLiteral("updatecheck")), KConfig::SimpleConfig);
            KConfigGroup group = config.group(QStringLiteral("Update"));
            group.writeEntry("Latest", QStringLiteral("99.0"));
            group.writeEntry("Skipped", QStringLiteral("99.0"));
            config.sync();
        }
        UpdateChecker checker;
        QVERIFY(checker.available().isEmpty());
        QCOMPARE(checker.latest(), QStringLiteral("99.0"));
        checker.skip(QString());
        QCOMPARE(checker.available(), QStringLiteral("99.0"));
        QCOMPARE(checker.page(), QUrl(QStringLiteral("https://github.com/nvaert1986/plasma-graphsync-linux/releases/tag/v99.0")));
    }
};

QTEST_GUILESS_MAIN(UpdateCheckTest)

#include "updatechecktest.moc"
