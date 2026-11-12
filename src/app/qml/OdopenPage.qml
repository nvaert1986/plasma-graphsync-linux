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

    // Names are text, never markup (messages show rich text)
    function plain(text) {
        return String(text ?? "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    function resolve() {
        info = backend.resolveOdopen(link);
        localPath = info.localPath ?? "";
    }

    title: i18n("Sync a Library")

    Component.onCompleted: {
        // The link may be for another account than the current one
        if (backend.signedIn || (backend.status.accounts ?? []).some(a => a.signedIn)) {
            resolve();
        }
    }

    // Back from signing in to the link's account, or from adding it: look again
    Connections {
        target: backend
        function onAccountChanged() {
            if (!page.started && (page.info.signInAccountId !== undefined || page.info.addAccountEmail !== undefined || page.info.siteName === undefined)) {
                page.resolve();
            }
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
            actions: [
                Kirigami.Action {
                    visible: page.info.signInAccountId !== undefined
                    icon.name: "im-user-online"
                    text: i18n("Sign In…")
                    onTriggered: {
                        backend.setCurrentAccount(page.info.signInAccountId);
                        applicationWindow().open("signIn", "");
                    }
                },
                Kirigami.Action {
                    visible: page.info.addAccountEmail !== undefined && page.info.signInAccountId === undefined
                    icon.name: "list-add-user"
                    text: i18n("Add Account…")
                    onTriggered: applicationWindow().open("addAccount", page.info.addAccountEmail)
                }
            ]
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Warning
            // Only when the link names someone (older links do not)
            visible: page.info.siteName !== undefined && page.info.accountMatches === false && (page.info.userEmail ?? "").length > 0
            text: i18n("This link was made for %1, but it syncs with %2. Syncing uses that account's access.", page.plain(page.info.userEmail), page.plain(page.info.accountName))
            actions: Kirigami.Action {
                visible: page.info.addAccountEmail !== undefined
                icon.name: "list-add-user"
                text: i18n("Add %1…", page.info.addAccountEmail ?? "")
                onTriggered: applicationWindow().open("addAccount", page.info.addAccountEmail)
            }
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
                // Only worth saying with more than one account
                visible: (backend.status.accounts ?? []).filter(a => a.signedIn).length > 1
                textFormat: Text.PlainText
                text: i18n("Syncs with %1", page.info.accountName ?? "")
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
                        webUrl: page.info.webUrl,
                        accountId: page.info.accountId ?? ""
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
