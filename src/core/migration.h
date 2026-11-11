// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace GraphSync::Migration
{

enum class Result {
    NotNeeded,
    Imported,
    Busy, // another process is importing, or the old daemon did not stop yet: try again later
    Failed, // could not copy; trying again will not help without the user
};

// Before 0.7.0 this client was called plasma-onedrive-linux (podld, ~/.config/podlrc,
// ~/.local/state/podl). An install of that version is taken over once per user: its daemon is
// masked and stopped, so two daemons never sync the same folders; then the sync state, the
// sign-in (KWallet) and the default handlers are moved over, the settings are copied, and a marker
// in the state folder records that it is done. Settings written meanwhile (graphsyncrc without
// setup) are kept as a backup and replaced by the old setup. Safe to call from the app, System
// Settings, the daemon and graphsyncctl at the same time; lockTimeoutMs is how long to wait for
// another process doing it.
Result importLegacyInstall(QString *message, int lockTimeoutMs = 60000);

// True while an install of plasma-onedrive-linux waits to be taken over (cheap: files only)
bool pending();

}
