// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace GraphSync::FolderIcon
{
// Gives a folder its icon in Dolphin and file dialogs through a .directory file (which is
// never synced). A folder that already has an icon, for example one the user chose, keeps it.
void apply(const QString &folder, const QString &iconName);
}
