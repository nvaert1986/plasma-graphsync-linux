// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The app's main window: the shared settings view, with the app's own pages for the rest.
Kirigami.Page {
    title: i18n("Graph Sync")
    padding: 0

    SettingsView {
        anchors.fill: parent
        api: backend
        section: startSection
        onOpenRequested: (mode, argument) => applicationWindow().open(mode, argument)
    }
}
