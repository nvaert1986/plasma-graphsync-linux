// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Opened by the Sync button in SharePoint (odopen:// link)
Kirigami.Page {
    id: page

    property string link
    property var info: ({})
    property string localPath
    property bool started: false

    function resolve() {
        info = backend.resolveOdopen(link);
        localPath = info.localPath ?? "";
    }

    title: i18n("Sync a Library")

    Component.onCompleted: {
        if (backend.signedIn) {
            resolve();
        }
    }

    FolderDialog {
        id: folderDialog
        title: i18n("Choose Where to Keep the Files")
        currentFolder: backend.fileUrl(backend.baseFolder)
        onAccepted: page.localPath = backend.childPath(selectedFolder, page.info.name)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            text: page.info.error ?? backend.lastError
            visible: text.length > 0
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Warning
            visible: page.info.siteName !== undefined && page.info.accountMatches === false
            text: i18n("This link was made for %1, but you are signed in as %2. Syncing uses your account's access.", page.info.userEmail ?? "", backend.userPrincipalName)
        }

        QQC2.Button {
            Layout.alignment: Qt.AlignHCenter
            visible: !backend.signedIn
            icon.name: "im-user-online"
            text: i18n("Sign In to Continue")
            onClicked: {
                if (backend.signIn()) {
                    page.resolve();
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: page.info.siteName !== undefined && !page.started
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Icon {
                Layout.alignment: Qt.AlignHCenter
                source: "folder-cloud"
                implicitWidth: Kirigami.Units.iconSizes.huge
                implicitHeight: Kirigami.Units.iconSizes.huge
            }
            Kirigami.Heading {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                level: 2
                // Names of sites and libraries are text, never markup
                textFormat: Text.PlainText
                text: page.info.folderName ? i18n("Sync the folder %1 from %2?", page.info.folderName, page.info.siteName) : i18n("Sync %1 from %2?", page.info.libraryName ?? "", page.info.siteName ?? "")
            }
            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("Files will be kept in:")
            }
            RowLayout {
                Layout.fillWidth: true
                QQC2.TextField {
                    Layout.fillWidth: true
                    text: page.localPath
                    onEditingFinished: page.localPath = text
                }
                QQC2.Button {
                    icon.name: "document-open-folder"
                    text: i18n("Change…")
                    onClicked: folderDialog.open()
                }
            }
            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: page.localPath.length > 0 && backend.folderHasFiles(page.localPath)
                type: Kirigami.MessageType.Warning
                text: i18n("This folder already contains files. They will be uploaded to the library.")
            }
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: page.started
            icon.name: "checkmark"
            text: i18n("Syncing has started")
            explanation: i18n("The files appear in %1. Follow progress in the system tray.", page.localPath)
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
                visible: !page.started
                enabled: page.info.driveId !== undefined && page.info.error === undefined && page.localPath.length > 0 && !backend.busy
                icon.name: "folder-sync"
                text: i18n("Sync")
                onClicked: {
                    const id = backend.addRoot({
                        name: page.info.name,
                        localPath: page.localPath,
                        driveId: page.info.driveId,
                        remoteRootId: page.info.remoteRootId ?? "",
                        kind: page.info.kind,
                        webUrl: page.info.webUrl
                    });
                    page.started = id !== "";
                }
            }
            QQC2.Button {
                visible: page.started
                icon.name: "document-open-folder"
                text: i18n("Open Folder")
                onClicked: Qt.openUrlExternally(backend.fileUrl(page.localPath))
            }
            QQC2.Button {
                icon.name: page.started ? "dialog-ok" : "dialog-cancel"
                text: page.started ? i18n("Close") : i18n("Cancel")
                onClicked: applicationWindow().closePage()
            }
        }
    }
}
