// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

// Entries in the Places panel of Dolphin and the file dialogs (user-places.xbel), like the
// "OneDrive - Org" and "Org" entries in the Windows navigation pane.
namespace GraphSync::Places
{

// Adds an entry once; when the user removes it, it stays removed
void ensure(const QString &path, const QString &label, const QString &iconName);

// Removes our entry for path, if there is one
void remove(const QString &path);

}
