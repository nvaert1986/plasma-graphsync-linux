// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Signs in another work account: of another organisation (a multi-tenant app registration its
// administrator approved, or that organisation's own registration), or another user. The new
// account becomes the one the app works with; its OneDrive and libraries are added from here.
Kirigami.Page {
    id: page

    // "details": e-mail address and registration; "signIn": waiting for the sign-in; "done"
    property string step: "details"
    // Pre-filled, for example from a Sync button link made for this account
    property string initialEmail

    title: i18n("Add Account")

    // Names from an organisation's directory are text, never markup
    function plain(text) {
        return String(text ?? "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    Component.onDestruction: {
        if (step !== "done") {
            backend.cancelNewAccount();
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            text: backend.adminConsentLink.length > 0
                ? i18n("This organization has not approved Graph Sync yet. An administrator of the organization opens the approval page once, signs in and accepts; then sign in again here.")
                : backend.lastError
            visible: backend.lastError.length > 0
            actions: [
                Kirigami.Action {
                    visible: backend.adminConsentLink.length > 0
                    icon.name: "internet-services"
                    text: i18n("Open Approval Page")
                    onTriggered: Qt.openUrlExternally(backend.adminConsentLink)
                },
                Kirigami.Action {
                    visible: backend.adminConsentLink.length > 0
                    icon.name: "edit-copy"
                    text: i18n("Copy Link")
                    onTriggered: backend.copyText(backend.adminConsentLink)
                }
            ]
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true
            visible: page.step !== "done"
            enabled: page.step === "details" && !backend.busy

            QQC2.TextField {
                id: emailField
                Kirigami.FormData.label: i18n("Work e-mail address:")
                text: page.initialEmail
                Layout.preferredWidth: Kirigami.Units.gridUnit * 18
                placeholderText: i18n("name@organization.com")
                inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
                onAccepted: continueButton.clicked()
            }
            QQC2.CheckBox {
                id: ownRegistration
                text: i18n("This organization has its own app registration")
            }
            QQC2.TextField {
                id: clientField
                Kirigami.FormData.label: i18n("Client ID:")
                visible: ownRegistration.checked
                Layout.preferredWidth: Kirigami.Units.gridUnit * 18
                placeholderText: "00000000-0000-0000-0000-000000000000"
                font.family: "monospace"
            }
            QQC2.Label {
                Layout.preferredWidth: Kirigami.Units.gridUnit * 22
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                text: ownRegistration.checked
                    ? i18n("The client ID of the app registration in that organization, from its IT administrator.")
                    : i18n("Signs in with the same app registration as your other accounts. Its organization must have approved it.")
            }
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: page.step !== "details"
            icon.name: page.step === "done" ? "checkmark" : "im-user-online"
            text: page.step === "done" ? i18n("Signed in as %1", page.plain(backend.userPrincipalName)) : i18n("Sign in with %1", page.plain(emailField.text.trim()))
            explanation: page.step === "done"
                ? i18nc("%1 is the organization", "Choose what to sync from %1.", page.plain(backend.organization))
                : i18n("Sign in in the browser with this work account.")
            helpfulAction: Kirigami.Action {
                enabled: page.step === "signIn"
                icon.name: "im-user-online"
                text: backend.busy ? (backend.deviceCode.length > 0 ? i18n("Waiting for the code…") : i18n("Waiting for the browser…")) : i18n("Sign In")
                onTriggered: {
                    if (!backend.busy && backend.signIn()) {
                        page.step = "done";
                    }
                }
            }
        }
        DeviceCodeBox {
            Layout.fillWidth: true
            visible: page.step === "signIn"
            onSignedIn: page.step = "done"
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            visible: page.step === "done"
            QQC2.Button {
                icon.name: "folder-cloud"
                text: i18nc("%1 is the organization", "Sync OneDrive - %1", page.plain(backend.organization))
                enabled: backend.daemonRunning
                onClicked: {
                    if (backend.addPersonal() !== "") {
                        enabled = false;
                    }
                }
            }
            QQC2.Button {
                icon.name: "list-add"
                text: i18n("Add a Library…")
                onClicked: applicationWindow().open("add", "")
            }
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
                id: continueButton
                visible: page.step === "details"
                enabled: emailField.text.trim().length > 0 && !backend.busy
                icon.name: "go-next"
                text: backend.busy ? i18n("Looking up the organization…") : i18n("Continue")
                onClicked: {
                    if (backend.startNewAccount(emailField.text, ownRegistration.checked ? clientField.text : "")) {
                        page.step = "signIn";
                    }
                }
            }
            QQC2.Button {
                visible: page.step === "signIn"
                enabled: !backend.busy
                icon.name: "go-previous"
                text: i18n("Back")
                onClicked: {
                    backend.cancelNewAccount();
                    page.step = "details";
                }
            }
            QQC2.Button {
                icon.name: page.step === "done" ? "dialog-ok" : "dialog-cancel"
                text: page.step === "done" ? i18n("Close") : i18n("Cancel")
                onClicked: applicationWindow().closePage()
            }
        }
    }
}
