// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Page {
    id: page

    property int step: 0
    property bool syncPersonal: true
    readonly property var titles: [i18n("Welcome"), i18n("Where to Keep Your Files"), i18n("Your OneDrive"), i18n("SharePoint Libraries"), i18n("Ready to Sync"), i18n("All Set")]

    function startSyncing() {
        let ok = true;
        if (syncPersonal) {
            const drive = backend.personalDrive();
            ok = drive.id !== undefined && backend.addRoot({
                name: i18nc("name of the personal OneDrive, %1 is the organization", "OneDrive - %1", backend.organization),
                localPath: backend.personalPath(),
                driveId: drive.id,
                kind: "personal",
                webUrl: drive.webUrl
            }) !== "";
        }
        for (const lib of picker.selected) {
            ok = backend.addRoot({
                name: lib.siteName + " - " + lib.libraryName,
                localPath: backend.libraryPath(lib.siteName, lib.libraryName),
                driveId: lib.driveId,
                kind: "library",
                webUrl: lib.webUrl
            }) !== "" && ok;
        }
        if (ok) {
            backend.finishSetup();
            step = 5;
        }
    }

    title: titles[step]

    Component.onCompleted: step = startStep

    FolderDialog {
        id: folderDialog
        title: i18n("Choose a Base Folder")
        currentFolder: backend.fileUrl(backend.baseFolder)
        onAccepted: backend.baseFolder = selectedFolder.toString()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            text: backend.lastError
            visible: backend.lastError.length > 0
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: page.step

            // 0: welcome and sign-in
            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Icon {
                    Layout.alignment: Qt.AlignHCenter
                    source: "io.github.nvaert1986.graphsync"
                    fallback: "folder-cloud"
                    implicitWidth: Kirigami.Units.iconSizes.enormous
                    implicitHeight: Kirigami.Units.iconSizes.enormous
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    text: i18n("Graph Sync keeps your OneDrive and SharePoint libraries in folders on this computer. Sign in with your work account to begin.")
                }
                // The organisation's app registration, unless an administrator set it already
                QQC2.Label {
                    Layout.fillWidth: true
                    visible: !backend.configured
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    text: i18n("First enter the client ID of your organization's app registration, which you get from your IT administrator (or paste the setup text you got). Your organization is found from your work e-mail address.")
                }
                Kirigami.FormLayout {
                    Layout.fillWidth: true
                    visible: !backend.configured
                    QQC2.TextField {
                        id: clientField
                        Kirigami.FormData.label: i18n("Client ID:")
                        implicitWidth: Kirigami.Units.gridUnit * 20
                        placeholderText: i18n("From your IT administrator")
                    }
                    QQC2.TextField {
                        id: tenantField
                        Kirigami.FormData.label: i18n("Work e-mail address:")
                        implicitWidth: Kirigami.Units.gridUnit * 20
                        placeholderText: i18nc("example e-mail address", "you@example.com")
                        inputMethodHints: Qt.ImhEmailCharactersOnly
                        onAccepted: continueButton.clicked()
                    }
                    QQC2.Button {
                        id: continueButton
                        enabled: !backend.busy && clientField.text.trim().length > 0
                        icon.name: "go-next"
                        text: backend.busy ? i18n("Looking up your organization…") : i18n("Continue")
                        onClicked: backend.saveAppRegistration(clientField.text, tenantField.text)
                    }
                }
                QQC2.Button {
                    Layout.alignment: Qt.AlignHCenter
                    visible: !backend.signedIn && backend.configured
                    enabled: !backend.busy
                    icon.name: "im-user-online"
                    text: backend.busy ? (backend.deviceCode.length > 0 ? i18n("Waiting for the code…") : i18n("Waiting for the browser…")) : i18n("Sign In")
                    onClicked: backend.signIn()
                }
                DeviceCodeBox {
                    Layout.fillWidth: true
                    visible: !backend.signedIn && backend.configured
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    visible: backend.signedIn
                    horizontalAlignment: Text.AlignHCenter
                    text: i18n("Signed in as %1 (%2)", backend.displayName, backend.userPrincipalName)
                }
                Item {
                    Layout.fillHeight: true
                }
            }

            // 1: base folder
            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: i18n("Choose the folder under which your synced files are kept. Each library gets its own folder inside it.")
                }
                RowLayout {
                    Layout.fillWidth: true
                    QQC2.TextField {
                        Layout.fillWidth: true
                        text: backend.baseFolder
                        onEditingFinished: backend.baseFolder = text
                    }
                    QQC2.Button {
                        icon.name: "document-open-folder"
                        text: i18n("Browse…")
                        onClicked: folderDialog.open()
                    }
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: i18n("Your OneDrive: %1\nSharePoint libraries: %2", backend.personalPath(), backend.libraryPath(i18n("Site"), i18n("Library")))
                }
                Item {
                    Layout.fillHeight: true
                }
            }

            // 2: personal OneDrive, on by default
            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing
                QQC2.Switch {
                    id: personalSwitch
                    checked: page.syncPersonal
                    text: i18nc("%1 is the organization", "Sync my OneDrive - %1", backend.organization)
                    onToggled: page.syncPersonal = checked
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: page.syncPersonal ? i18n("Your own files will be kept in %1.", backend.personalPath()) : i18n("Your own OneDrive will not be synced. You can turn it on later in the settings.")
                }
                Item {
                    Layout.fillHeight: true
                }
            }

            // 3: libraries
            LibraryPicker {
                id: picker
            }

            // 4: summary
            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: page.syncPersonal || picker.selected.length > 0 ? i18n("The following will be synced:") : i18n("Nothing selected yet. Go back to pick your OneDrive or a library, or finish and add libraries later.")
                }
                Repeater {
                    model: (page.syncPersonal ? [{
                                name: i18nc("%1 is the organization", "OneDrive - %1", backend.organization),
                                path: backend.personalPath()
                            }] : []).concat(picker.selected.map(lib => ({
                                name: lib.siteName + " - " + lib.libraryName,
                                path: backend.libraryPath(lib.siteName, lib.libraryName)
                            })))
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.largeSpacing
                        Kirigami.Icon {
                            source: "folder-cloud"
                            implicitWidth: Kirigami.Units.iconSizes.medium
                            implicitHeight: Kirigami.Units.iconSizes.medium
                        }
                        ColumnLayout {
                            spacing: 0
                            QQC2.Label {
                                Layout.fillWidth: true
                                font.bold: true
                                text: modelData.name
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                                opacity: 0.7
                                text: modelData.path
                            }
                        }
                    }
                }
                Item {
                    Layout.fillHeight: true
                }
            }

            // 5: done
            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing
                Kirigami.PlaceholderMessage {
                    id: allSet
                    // Checked when this page shows: a package installed during this session is
                    // not in the panel yet
                    readonly property bool panelRestartNeeded: page.step === 5 && backend.trayNeedsPanelRestart()
                    Layout.fillWidth: true
                    icon.name: "checkmark"
                    text: i18n("You're all set")
                    explanation: allSet.panelRestartNeeded
                        ? i18n("Syncing has started. The Graph Sync icon appears in the system tray after you log out and back in, or now with the button below (the panel restarts briefly). To sync another library later, use the Sync button in SharePoint.")
                        : i18n("Syncing has started. Look for the Graph Sync icon in the system tray to follow progress. To sync another library later, use the Sync button in SharePoint.")
                    // The placeholder shows its button when the action is enabled
                    helpfulAction: Kirigami.Action {
                        enabled: allSet.panelRestartNeeded
                        icon.name: "view-refresh"
                        text: i18n("Show the Tray Icon (Restarts the Plasma Panel)")
                        onTriggered: {
                            backend.restartPanel();
                            enabled = false;
                        }
                    }
                }
                Item {
                    Layout.fillHeight: true
                }
            }
        }
    }

    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            QQC2.Button {
                visible: page.step > 0 && page.step < 5
                icon.name: "go-previous"
                text: i18n("Back")
                onClicked: page.step -= 1
            }
            Item {
                Layout.fillWidth: true
            }
            QQC2.Button {
                visible: page.step < 4
                enabled: backend.signedIn && !backend.busy
                icon.name: "go-next"
                text: i18n("Next")
                onClicked: page.step += 1
            }
            QQC2.Button {
                visible: page.step === 4
                enabled: !backend.busy
                icon.name: "folder-sync"
                text: i18n("Start Syncing")
                onClicked: page.startSyncing()
            }
            QQC2.Button {
                visible: page.step === 5
                icon.name: "dialog-ok"
                text: i18n("Close")
                // Setup is saved by now; the tray takes over. (open() ignores everything while
                // the wizard is shown, so asking it for the settings did nothing.)
                onClicked: applicationWindow().close()
            }
        }
    }
}
