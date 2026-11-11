// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Page {
    id: page

    property bool done: false

    title: i18n("Add a SharePoint Library")

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            text: backend.lastError
            visible: backend.lastError.length > 0
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Positive
            text: i18n("Syncing has started.")
            visible: page.done
        }
        LibraryPicker {
            id: picker
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: backend.signedIn
        }
        QQC2.Button {
            Layout.alignment: Qt.AlignHCenter
            visible: !backend.signedIn
            icon.name: "im-user-online"
            text: i18n("Sign In")
            onClicked: backend.signIn()
        }
    }

    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            QQC2.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                opacity: 0.7
                text: picker.selected.length > 0 ? i18np("%1 library selected", "%1 libraries selected", picker.selected.length) : ""
            }
            QQC2.Button {
                enabled: picker.selected.length > 0 && !backend.busy
                icon.name: "folder-sync"
                text: i18n("Sync")
                onClicked: {
                    let ok = true;
                    for (const lib of picker.selected) {
                        ok = backend.addRoot({
                            name: lib.siteName + " - " + lib.libraryName,
                            localPath: backend.libraryPath(lib.siteName, lib.libraryName),
                            driveId: lib.driveId,
                            kind: "library",
                            webUrl: lib.webUrl
                        }) !== "" && ok;
                    }
                    page.done = ok;
                    if (ok) {
                        picker.selected = [];
                    }
                }
            }
            QQC2.Button {
                icon.name: "dialog-close"
                text: i18n("Close")
                onClicked: applicationWindow().closePage()
            }
        }
    }
}
