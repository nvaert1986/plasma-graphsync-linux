// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "updatecheck.h"
#include "config.h"

#include <QDir>

namespace GraphSync
{

namespace UpdateCheck
{
bool compiledIn()
{
#ifdef GRAPHSYNC_UPDATE_CHECK
    return true;
#else
    return false;
#endif
}

QString currentVersion()
{
#ifdef GRAPHSYNC_TESTING
    // Tests only (Debug builds of this project): pretend to be an older version
    const QString test = qEnvironmentVariable("GRAPHSYNC_TEST_CURRENT_VERSION");
    if (!test.isEmpty()) {
        return test;
    }
#endif
    return QStringLiteral(GRAPHSYNC_VERSION);
}

GraphCore::UpdateCheckSettings settings()
{
    GraphCore::UpdateCheckSettings settings;
    settings.repository = QStringLiteral("nvaert1986/plasma-graphsync-linux");
    settings.currentVersion = currentVersion();
    // The same file as before 0.12.0: the last answer and a skipped version stay
    settings.stateFile = QDir(Config::stateDirectory()).filePath(QStringLiteral("updatecheck"));
    settings.userAgent = QStringLiteral("plasma-graphsync-linux/%1").arg(QStringLiteral(GRAPHSYNC_VERSION));
#ifdef GRAPHSYNC_TESTING
    // Tests only: ask a local server instead of GitHub
    if (const QString test = qEnvironmentVariable("GRAPHSYNC_TEST_UPDATE_URL"); !test.isEmpty()) {
        settings.latestUrl = QUrl(test);
    }
#endif
    return settings;
}
}

}
