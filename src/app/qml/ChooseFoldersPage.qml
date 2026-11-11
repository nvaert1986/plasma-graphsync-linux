// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Folder tree with checkboxes; unticked folders become the root's excluded paths.
Kirigami.Page {
    id: page

    property string rootId
    property var root: ({})
    property var excluded: []
    property bool saved: false

    function isExcluded(path) {
        return excluded.some(e => path === e || path.startsWith(e + "/"));
    }

    function loadChildren(index, itemId, parentPath, depth) {
        const children = backend.childFolders(root.driveId, itemId);
        let insertAt = index + 1;
        for (const child of children) {
            const path = parentPath.length > 0 ? parentPath + "/" + child.name : child.name;
            folders.insert(insertAt++, {
                itemId: child.id,
                name: child.name,
                path: path,
                depth: depth,
                expanded: false,
                hasChildren: child.childCount > 0
            });
        }
    }

    function collapse(index) {
        const depth = folders.get(index).depth;
        while (index + 1 < folders.count && folders.get(index + 1).depth > depth) {
            folders.remove(index + 1);
        }
    }

    function setChecked(path, on) {
        let next = excluded.filter(e => e !== path && !e.startsWith(path + "/"));
        if (!on) {
            next.push(path);
        }
        excluded = next;
    }

    title: i18n("Choose Folders")

    Component.onCompleted: {
        root = backend.root(rootId);
        excluded = root.excludedPaths ?? [];
        if (root.driveId) {
            loadChildren(-1, root.remoteRootId ?? "", "", 0);
        }
    }

    ListModel {
        id: folders
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: i18n("Choose which folders of %1 are kept on this computer. Unticked folders stay online, but their local copies are moved to the Trash.", page.root.name ?? "")
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            text: backend.lastError
            visible: backend.lastError.length > 0
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Positive
            text: i18n("Saved. Changes are applied with the next sync.")
            visible: page.saved
        }

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                model: folders
                clip: true

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    visible: list.count === 0 && !backend.busy
                    icon.name: "folder"
                    text: i18n("This library has no folders")
                }

                delegate: RowLayout {
                    id: row
                    required property int index
                    required property string itemId
                    required property string name
                    required property string path
                    required property int depth
                    required property bool expanded
                    required property bool hasChildren
                    readonly property bool parentExcluded: page.excluded.some(e => row.path.startsWith(e + "/"))

                    width: ListView.view.width
                    spacing: 0

                    Item {
                        implicitWidth: row.depth * Kirigami.Units.gridUnit
                    }
                    QQC2.ToolButton {
                        icon.name: row.expanded ? "go-down-symbolic" : "go-next-symbolic"
                        opacity: row.hasChildren ? 1 : 0
                        enabled: row.hasChildren && !page.isExcluded(row.path)
                        onClicked: {
                            if (row.expanded) {
                                page.collapse(row.index);
                            } else {
                                page.loadChildren(row.index, row.itemId, row.path, row.depth + 1);
                            }
                            folders.setProperty(row.index, "expanded", !row.expanded);
                        }
                    }
                    QQC2.CheckBox {
                        Layout.fillWidth: true
                        enabled: !row.parentExcluded
                        checked: !page.isExcluded(row.path)
                        icon.name: "folder"
                        text: row.name
                        onToggled: page.setChecked(row.path, checked)
                    }
                }
            }
        }
    }

    footer: QQC2.ToolBar {
        contentItem: RowLayout {
            Item {
                Layout.fillWidth: true
            }
            QQC2.Button {
                icon.name: "document-save"
                text: i18n("Save")
                onClicked: page.saved = backend.setExcludedFolders(page.rootId, page.excluded)
            }
            QQC2.Button {
                icon.name: "dialog-close"
                text: i18n("Close")
                onClicked: applicationWindow().closePage()
            }
        }
    }
}
