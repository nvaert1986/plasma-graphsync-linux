// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

namespace GraphSync::DolphinPreviews
{

// Our thumbnail plugin for online-only files, as Dolphin lists it in [PreviewSettings] Plugins
inline constexpr QLatin1StringView plugin("graphsyncthumbnail");

// Whether Dolphin shows our previews: without a list of its own Dolphin uses every plugin that is
// on by default, ours included. configName is "dolphinrc" (read fresh each time: Dolphin may have
// changed it meanwhile).
bool enabled(const QString &configName = QStringLiteral("dolphinrc"));

// Adds our plugin to Dolphin's list or takes it out. Without a list yet, Dolphin's defaults
// (defaults, from KIO) become the list first, so nothing else changes for the user.
void setEnabled(bool on, const QStringList &defaults, const QString &configName = QStringLiteral("dolphinrc"));

}
