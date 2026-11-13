// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dolphinpreviews.h"

#include <KConfig>
#include <KConfigGroup>

#include <QFile>
#include <QStandardPaths>
#include <QTest>

using namespace GraphSync;

namespace
{
const QString name = QStringLiteral("dolphinpreviewstestrc");

QStringList plugins()
{
    const KConfig config(name, KConfig::NoGlobals);
    return KConfigGroup(&config, QStringLiteral("PreviewSettings")).readEntry("Plugins", QStringList());
}

bool hasList()
{
    const KConfig config(name, KConfig::NoGlobals);
    return KConfigGroup(&config, QStringLiteral("PreviewSettings")).hasKey("Plugins");
}
}

class DolphinPreviewsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QLatin1Char('/') + name);
    }

    void withoutAListOursIsOn()
    {
        QVERIFY(DolphinPreviews::enabled(name));
        // Switching it on changes nothing: no list is written
        DolphinPreviews::setEnabled(true, {QStringLiteral("imagethumbnail")}, name);
        QVERIFY(!hasList());
    }

    void switchingOffKeepsTheDefaults()
    {
        DolphinPreviews::setEnabled(false, {QStringLiteral("imagethumbnail"), QStringLiteral("graphsyncthumbnail")}, name);
        QCOMPARE(plugins(), QStringList{QStringLiteral("imagethumbnail")});
        QVERIFY(!DolphinPreviews::enabled(name));
    }

    void anOwnListGetsOursAddedAndRemoved()
    {
        {
            KConfig config(name, KConfig::NoGlobals);
            KConfigGroup(&config, QStringLiteral("PreviewSettings")).writeEntry("Plugins", QStringList{QStringLiteral("a"), QStringLiteral("b")});
        }
        QVERIFY(!DolphinPreviews::enabled(name));
        DolphinPreviews::setEnabled(true, {QStringLiteral("ignored")}, name);
        QCOMPARE(plugins(), (QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("graphsyncthumbnail")}));
        QVERIFY(DolphinPreviews::enabled(name));
        // Twice on: still once in the list
        DolphinPreviews::setEnabled(true, {}, name);
        QCOMPARE(plugins().count(QStringLiteral("graphsyncthumbnail")), 1);
        DolphinPreviews::setEnabled(false, {}, name);
        QCOMPARE(plugins(), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
    }
};

QTEST_GUILESS_MAIN(DolphinPreviewsTest)

#include "dolphinpreviewstest.moc"
