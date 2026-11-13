// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dolphinpreviews.h"

#include <KConfig>
#include <KConfigGroup>

namespace GraphSync::DolphinPreviews
{

bool enabled(const QString &configName)
{
    const KConfig dolphin(configName, KConfig::NoGlobals);
    const KConfigGroup group(&dolphin, QStringLiteral("PreviewSettings"));
    return !group.hasKey("Plugins") || group.readEntry("Plugins", QStringList()).contains(QString(plugin));
}

void setEnabled(bool on, const QStringList &defaults, const QString &configName)
{
    if (on == enabled(configName)) {
        return;
    }
    KConfig dolphin(configName, KConfig::NoGlobals);
    KConfigGroup group(&dolphin, QStringLiteral("PreviewSettings"));
    QStringList plugins = group.hasKey("Plugins") ? group.readEntry("Plugins", QStringList()) : defaults;
    plugins.removeAll(QString(plugin));
    if (on) {
        plugins.append(QString(plugin));
    }
    group.writeEntry("Plugins", plugins);
    group.sync();
}

}
