// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Sign-in with a code: offered next to the browser sign-in for when the browser cannot come
// back to the app (strict browser policies, a remote session). Shows the code while waiting.
ColumnLayout {
    id: box

    // Emitted after a successful sign-in
    signal signedIn()

    spacing: Kirigami.Units.smallSpacing

    QQC2.Button {
        Layout.alignment: Qt.AlignHCenter
        // Builds without the browser sign-in use the code for every Sign In button already
        visible: !backend.busy && backend.browserSignIn
        flat: true
        icon.name: "dialog-password"
        text: i18n("Sign In with a Code Instead")
        onClicked: {
            if (backend.signInWithCode()) {
                box.signedIn();
            }
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: backend.deviceCode.length > 0
        type: Kirigami.MessageType.Information
        text: i18n("Open %1 on any device, enter the code below and sign in with your work account. This window continues by itself.", backend.deviceCodeAddress)
        actions: [
            Kirigami.Action {
                icon.name: "internet-services"
                text: i18n("Open Page")
                onTriggered: Qt.openUrlExternally(backend.deviceCodeAddress)
            },
            Kirigami.Action {
                icon.name: "dialog-cancel"
                text: i18n("Cancel")
                onTriggered: backend.cancelSignIn()
            }
        ]
    }
    RowLayout {
        Layout.alignment: Qt.AlignHCenter
        visible: backend.deviceCode.length > 0
        QQC2.TextField {
            id: codeField
            readOnly: true
            text: backend.deviceCode
            font.family: "monospace"
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.6
            horizontalAlignment: Text.AlignHCenter
            implicitWidth: Kirigami.Units.gridUnit * 10
        }
        QQC2.Button {
            icon.name: "edit-copy"
            text: i18n("Copy Code")
            onClicked: {
                codeField.selectAll();
                codeField.copy();
                codeField.deselect();
            }
        }
    }
}
