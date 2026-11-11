// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Shown while an install of plasma-onedrive-linux (this client before 0.7.0) is taken over,
// instead of the setup wizard: going through the wizard meanwhile would start a new setup
Kirigami.Page {
    id: page

    title: i18n("Taking Over Your Settings")

    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        spacing: Kirigami.Units.largeSpacing

        QQC2.BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            visible: backend.importState === "running"
            running: visible
        }
        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            icon.name: backend.importState === "failed" ? "data-warning" : "folder-sync"
            text: backend.importState === "failed" ? i18n("Your settings were not taken over yet") : i18n("Taking over your settings from plasma-onedrive-linux…")
            explanation: backend.importState === "failed"
                ? backend.importMessage
                : i18n("This client was called plasma-onedrive-linux before. Your folders, sync state and sign-in are moved over once; nothing is downloaded again.")
            // The placeholder shows its button when the action is enabled
            helpfulAction: Kirigami.Action {
                enabled: backend.importState === "failed"
                icon.name: "view-refresh"
                text: i18n("Try Again")
                onTriggered: backend.retryImport()
            }
        }
    }
}
