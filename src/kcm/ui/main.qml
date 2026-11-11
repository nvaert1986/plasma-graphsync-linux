// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Layouts
import org.kde.kcmutils as KCMUtils
import org.kde.kirigami as Kirigami

// The same settings as the app's window. Pages the view cannot show here (add a library,
// choose folders, sign in) open in the app.
KCMUtils.AbstractKCM {
    padding: 0
    framedView: false

    // Taking over plasma-onedrive-linux (this client before 0.7.0) runs in the background
    header: Kirigami.InlineMessage {
        visible: kcm.backend.importState !== ""
        position: Kirigami.InlineMessage.Position.Header
        type: kcm.backend.importState === "failed" ? Kirigami.MessageType.Warning : Kirigami.MessageType.Information
        text: kcm.backend.importState === "failed" ? kcm.backend.importMessage : i18n("Taking over your settings from plasma-onedrive-linux…")
        actions: Kirigami.Action {
            visible: kcm.backend.importState === "failed"
            icon.name: "view-refresh"
            text: i18n("Try Again")
            onTriggered: kcm.backend.retryImport()
        }
    }

    SettingsView {
        anchors.fill: parent
        api: kcm.backend
        onOpenRequested: (mode, argument) => {
            switch (mode) {
            case "add":
                kcm.backend.launchApp(["--add"]);
                break;
            case "chooseFolders":
                kcm.backend.launchApp(["--choose-folders", argument]);
                break;
            case "signIn":
                kcm.backend.launchApp(["--sign-in"]);
                break;
            }
        }
    }
}
