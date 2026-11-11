// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Page {
    id: page

    property bool done: false

    title: i18n("Sign In")

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            text: backend.lastError
            visible: backend.lastError.length > 0
        }
        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            icon.name: page.done ? "checkmark" : "im-user-online"
            text: page.done ? i18n("Signed in as %1", backend.userPrincipalName) : i18n("Sign in to continue syncing")
            explanation: page.done ? i18n("Syncing continues in the background.") : i18n("Your session expired. Sign in with your work account in the browser.")
            helpfulAction: Kirigami.Action {
                // The placeholder shows its button when the action is enabled; visible is ignored.
                // While waiting it stays, showing what it waits for; a second click does nothing
                enabled: !page.done
                icon.name: "im-user-online"
                text: backend.busy ? (backend.deviceCode.length > 0 ? i18n("Waiting for the code…") : i18n("Waiting for the browser…")) : i18n("Sign In")
                onTriggered: {
                    if (!backend.busy) {
                        page.done = backend.signIn();
                    }
                }
            }
        }
        DeviceCodeBox {
            Layout.fillWidth: true
            visible: !page.done
            onSignedIn: page.done = true
        }
        Item {
            Layout.fillHeight: true
        }
    }

    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            Item {
                Layout.fillWidth: true
            }
            QQC2.Button {
                icon.name: "dialog-close"
                text: i18n("Close")
                onClicked: applicationWindow().closePage()
            }
        }
    }
}
