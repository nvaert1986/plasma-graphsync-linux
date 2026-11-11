#!/bin/sh
# SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Extracts translatable strings into $podir (KDE convention; XGETTEXT is set by the caller)
$XGETTEXT $(find src/core src/graphsyncd src/graphsyncctl src/app src/dolphin -name '*.cpp' -o -name '*.h') -o $podir/plasma-graphsync-linux.pot
$XGETTEXT --join-existing -L JavaScript $(find src/app -name '*.qml') -o $podir/plasma-graphsync-linux.pot
# The System Settings page shares the app's Backend and settings view, under its own catalog
$XGETTEXT src/app/backend.cpp -o $podir/kcm_graphsync.pot
$XGETTEXT --join-existing -L JavaScript $(find src/kcm -name '*.qml') src/app/qml/SettingsView.qml -o $podir/kcm_graphsync.pot
$XGETTEXT -L JavaScript $(find plasmoid -name '*.qml') -o $podir/plasma_applet_io.github.nvaert1986.graphsync.tray.pot
