// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// OneDrive's Version history: open, save a copy of, or restore an earlier version.
Kirigami.ScrollablePage {
    id: page

    property string path
    property var info: ({})
    property var versions: []
    property string notice
    property var pending: ({})

    function reload() {
        versions = backend.versions(path);
    }

    title: i18n("Version History of “%1”", info.name ?? "")

    Component.onCompleted: {
        info = backend.itemInfo(path);
        if (info.ok) {
            reload();
        }
    }

    header: ColumnLayout {
        spacing: 0
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            text: backend.lastError
            visible: backend.lastError.length > 0
            position: Kirigami.InlineMessage.Position.Header
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Positive
            text: page.notice
            visible: page.notice.length > 0
            position: Kirigami.InlineMessage.Position.Header
        }
    }

    ListView {
        id: list
        model: page.versions

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: list.count === 0 && !backend.busy && page.info.ok === true
            icon.name: "view-history"
            text: page.info.isFolder ? i18n("Folders have no version history") : i18n("No earlier versions")
        }

        delegate: QQC2.ItemDelegate {
            id: row
            required property var modelData
            width: ListView.view.width
            hoverEnabled: true

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    QQC2.Label {
                        Layout.fillWidth: true
                        font.bold: row.modelData.current
                        text: row.modelData.current ? i18n("Version %1 (current)", row.modelData.id) : i18n("Version %1", row.modelData.id)
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        text: [row.modelData.modified.toLocaleString(Qt.locale(), Locale.ShortFormat), row.modelData.modifiedBy, row.modelData.sizeText].filter(t => t).join("  ·  ")
                    }
                }
                QQC2.Button {
                    icon.name: "document-open"
                    text: i18n("Open")
                    enabled: !backend.busy
                    onClicked: {
                        // The current version is the synced file itself
                        if (row.modelData.current) {
                            Qt.openUrlExternally(backend.fileUrl(page.path));
                        } else {
                            backend.openVersion(page.path, row.modelData.id);
                        }
                    }
                }
                QQC2.Button {
                    icon.name: "document-save-as"
                    text: i18n("Save a Copy…")
                    enabled: !backend.busy
                    visible: !row.modelData.current
                    onClicked: {
                        page.pending = row.modelData;
                        saveDialog.open();
                    }
                }
                QQC2.Button {
                    icon.name: "edit-undo"
                    text: i18n("Restore")
                    enabled: !backend.busy
                    visible: !row.modelData.current
                    onClicked: {
                        page.pending = row.modelData;
                        restoreDialog.openedBy = backend.openedBy(page.path);
                        restoreDialog.open();
                    }
                }
            }
        }
    }

    FileDialog {
        id: saveDialog
        fileMode: FileDialog.SaveFile
        title: i18n("Save Version %1", page.pending.id ?? "")
        currentFolder: backend.fileUrl(page.path.substring(0, page.path.lastIndexOf("/")))
        selectedFile: {
            const name = page.info.name ?? "";
            const dot = name.lastIndexOf(".");
            const copy = dot > 0 ? i18nc("file name of a saved old version: name, version, extension", "%1 (version %2)%3", name.substring(0, dot), page.pending.id ?? "", name.substring(dot))
                                 : i18nc("file name of a saved old version: name, version, extension", "%1 (version %2)%3", name, page.pending.id ?? "", "");
            return currentFolder + "/" + copy;
        }
        onAccepted: {
            if (backend.saveVersionAs(page.path, page.pending.id, selectedFile)) {
                page.notice = i18n("Version %1 saved.", page.pending.id);
            }
        }
    }

    Kirigami.PromptDialog {
        id: restoreDialog
        property string openedBy
        title: i18n("Restore Version %1?", page.pending.id ?? "")
        subtitle: {
            let text = i18n("The online file goes back to this version. The current version stays in the history, so you can switch back.");
            if (openedBy.length > 0) {
                text += "\n\n" + i18n("%1 has this file open. Close it first, or saving it there later will cause a conflict.", openedBy);
            }
            return text;
        }
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                icon.name: "edit-undo"
                text: i18n("Restore")
                onTriggered: {
                    restoreDialog.close();
                    if (backend.restoreVersion(page.path, page.pending.id)) {
                        page.notice = i18n("Version %1 restored. The file on this computer is updated in a moment.", page.pending.id);
                        page.reload();
                    }
                }
            }
        ]
    }

    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            QQC2.BusyIndicator {
                running: backend.busy
                visible: running
                Layout.preferredHeight: Kirigami.Units.iconSizes.medium
            }
            Item {
                Layout.fillWidth: true
            }
            QQC2.Button {
                icon.name: "internet-services"
                text: i18n("View Online")
                enabled: (page.info.webUrl ?? "").length > 0
                onClicked: Qt.openUrlExternally(page.info.webUrl)
            }
            QQC2.Button {
                icon.name: "dialog-close"
                text: i18n("Close")
                onClicked: applicationWindow().closePage()
            }
        }
    }
}
