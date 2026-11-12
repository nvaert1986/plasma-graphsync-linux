// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// OneDrive's Share dialog: invite people, a link with settings, and who has access.
Kirigami.ScrollablePage {
    id: page

    property string path
    property var info: ({})
    property var access: []
    property var recipients: []
    property var suggestions: []
    property bool peopleNotAllowed: false
    property string notice
    property string copiedUrl

    function roleText(role) {
        switch (role) {
        case "owner":
            return i18n("Owner");
        case "write":
            return i18n("Can edit");
        default:
            return i18n("Can view");
        }
    }

    function linkTitle(entry) {
        switch (entry.scope) {
        case "anonymous":
            return i18n("Anyone with the link");
        case "organization":
            return info.organization ? i18n("People in %1 with the link", info.organization) : i18n("People in your organisation with the link");
        default:
            return i18n("Specific people with the link");
        }
    }

    function isEmail(text) {
        return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(text.trim());
    }

    function addRecipient(name, email) {
        email = email.trim();
        if (email.length === 0 || recipients.some(r => r.email.toLowerCase() === email.toLowerCase())) {
            return;
        }
        recipients = recipients.concat([{ name: name || email, email: email }]);
        peopleField.text = "";
        suggestions = [];
    }

    function reloadAccess() {
        access = backend.permissions(path);
    }

    title: i18n("Share “%1”", info.name ?? "")

    Component.onCompleted: {
        info = backend.itemInfo(path);
        if (info.ok) {
            reloadAccess();
        }
    }

    Timer {
        id: searchTimer
        interval: 400
        onTriggered: {
            const result = backend.searchPeople(page.path, peopleField.text);
            page.peopleNotAllowed = result.notAllowed;
            page.suggestions = result.people;
        }
    }

    ColumnLayout {
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
            text: page.notice
            visible: page.notice.length > 0
        }

        // --- Invite people
        Kirigami.Heading {
            level: 3
            text: i18n("Invite people")
        }
        RowLayout {
            Layout.fillWidth: true
            QQC2.TextField {
                id: peopleField
                Layout.fillWidth: true
                enabled: page.info.ok === true
                placeholderText: page.peopleNotAllowed ? i18n("Type an e-mail address") : i18n("Add a name or e-mail address")
                onTextEdited: {
                    if (!page.peopleNotAllowed && text.trim().length >= 2) {
                        searchTimer.restart();
                    } else {
                        page.suggestions = [];
                    }
                }
                onAccepted: addButton.add()
            }
            QQC2.Button {
                id: addButton
                // A typed address, or the first suggestion for a typed name
                function add() {
                    if (page.isEmail(peopleField.text)) {
                        page.addRecipient("", peopleField.text);
                    } else if (page.suggestions.length > 0) {
                        page.addRecipient(page.suggestions[0].name, page.suggestions[0].email);
                    }
                }
                enabled: page.isEmail(peopleField.text) || page.suggestions.length > 0
                icon.name: "list-add-user"
                text: i18n("Add")
                onClicked: add()
            }
        }
        Repeater {
            model: page.suggestions
            delegate: QQC2.ItemDelegate {
                required property var modelData
                Layout.fillWidth: true
                icon.name: "user"
                text: modelData.name + "  ·  " + modelData.email
                onClicked: page.addRecipient(modelData.name, modelData.email)
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            visible: page.recipients.length > 0
            Repeater {
                model: page.recipients
                delegate: Kirigami.Chip {
                    required property var modelData
                    text: modelData.name
                    icon.name: "user"
                    closable: true
                    checkable: false
                    onRemoved: page.recipients = page.recipients.filter(r => r.email !== modelData.email)
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            QQC2.ComboBox {
                id: inviteRole
                textRole: "text"
                valueRole: "value"
                model: [
                    { text: i18n("Can edit"), value: "write" },
                    { text: i18n("Can view"), value: "read" }
                ]
            }
            QQC2.CheckBox {
                id: notifyBox
                Layout.fillWidth: true
                checked: true
                text: i18n("Send an e-mail invitation")
            }
        }
        QQC2.TextArea {
            id: messageField
            Layout.fillWidth: true
            visible: notifyBox.checked
            placeholderText: i18n("Add a message (optional)")
            wrapMode: TextEdit.Wrap
        }
        QQC2.Button {
            // An address still in the field counts as added, so typing one and pressing Send works
            enabled: page.info.ok === true && !backend.busy && (page.recipients.length > 0 || page.isEmail(peopleField.text))
            icon.name: "document-send"
            text: notifyBox.checked ? i18n("Send") : i18n("Give Access")
            onClicked: {
                if (page.isEmail(peopleField.text)) {
                    page.addRecipient("", peopleField.text);
                }
                if (backend.invite(page.path, page.recipients.map(r => r.email), inviteRole.currentValue, notifyBox.checked, messageField.text)) {
                    page.notice = i18np("Shared with 1 person.", "Shared with %1 people.", page.recipients.length);
                    page.recipients = [];
                    messageField.text = "";
                    page.reloadAccess();
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        // --- Copy a link
        Kirigami.Heading {
            level: 3
            text: i18n("Copy a link")
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            QQC2.Label {
                text: i18n("Who:")
            }
            QQC2.ComboBox {
                id: linkScope
                Layout.fillWidth: true
                textRole: "text"
                valueRole: "value"
                model: [
                    { text: page.info.organization ? i18n("People in %1", page.info.organization) : i18n("People in your organisation"), value: "organization" },
                    { text: i18n("Anyone with the link"), value: "anonymous" }
                ]
            }
            QQC2.Label {
                text: i18n("Permission:")
            }
            QQC2.ComboBox {
                id: linkType
                Layout.fillWidth: true
                textRole: "text"
                valueRole: "value"
                currentIndex: backend.defaultLinkType === "edit" ? 1 : 0
                model: [
                    { text: i18n("Can view"), value: "view" },
                    { text: i18n("Can edit"), value: "edit" }
                ]
            }
            QQC2.Label {
                // Expiry only applies to links for anyone
                visible: linkScope.currentValue === "anonymous"
                text: i18n("Expires:")
            }
            QQC2.ComboBox {
                id: linkExpiry
                Layout.fillWidth: true
                visible: linkScope.currentValue === "anonymous"
                textRole: "text"
                valueRole: "value"
                model: [
                    { text: i18n("Never"), value: 0 },
                    { text: i18n("In 1 day"), value: 1 },
                    { text: i18n("In 7 days"), value: 7 },
                    { text: i18n("In 30 days"), value: 30 }
                ]
            }
        }
        RowLayout {
            Layout.fillWidth: true
            QQC2.Button {
                enabled: page.info.ok === true && !backend.busy
                icon.name: "edit-copy"
                text: i18n("Copy Link")
                onClicked: {
                    const link = backend.createLink(page.path, linkType.currentValue, linkScope.currentValue,
                                                     linkScope.currentValue === "anonymous" ? linkExpiry.currentValue : 0);
                    if (link.ok && backend.copyText(link.url)) {
                        page.copiedUrl = link.url;
                        page.notice = i18n("Link copied.");
                        page.reloadAccess();
                    }
                }
            }
            QQC2.TextField {
                Layout.fillWidth: true
                visible: page.copiedUrl.length > 0
                readOnly: true
                text: page.copiedUrl
                onActiveFocusChanged: if (activeFocus) selectAll()
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        // --- Who has access
        Kirigami.Heading {
            level: 3
            text: i18n("Who has access")
        }
        Repeater {
            model: page.access
            delegate: RowLayout {
                id: accessRow
                required property var modelData
                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing

                Kirigami.Icon {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    source: accessRow.modelData.kind === "link" ? "link" : (accessRow.modelData.kind === "group" ? "group" : "user")
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    QQC2.Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: accessRow.modelData.kind === "link" ? page.linkTitle(accessRow.modelData) : accessRow.modelData.name
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        text: {
                            let parts = [page.roleText(accessRow.modelData.role)];
                            if (accessRow.modelData.detail) {
                                parts.push(accessRow.modelData.detail);
                            }
                            if (accessRow.modelData.expires && !isNaN(accessRow.modelData.expires)) {
                                parts.push(i18n("expires %1", accessRow.modelData.expires.toLocaleDateString(Qt.locale(), Locale.ShortFormat)));
                            }
                            return parts.join("  ·  ");
                        }
                    }
                }
                QQC2.ToolButton {
                    visible: accessRow.modelData.kind === "link" && accessRow.modelData.url
                    icon.name: "edit-copy"
                    text: i18n("Copy")
                    display: QQC2.AbstractButton.IconOnly
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    onClicked: {
                        if (backend.copyText(accessRow.modelData.url)) {
                            page.notice = i18n("Link copied.");
                        }
                    }
                }
                QQC2.ToolButton {
                    visible: accessRow.modelData.removable
                    enabled: !backend.busy
                    icon.name: "list-remove"
                    text: i18n("Remove Link")
                    display: QQC2.AbstractButton.IconOnly
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    onClicked: {
                        if (backend.removePermission(page.path, accessRow.modelData.id)) {
                            page.notice = i18n("The link no longer works.");
                            page.reloadAccess();
                        }
                    }
                }
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            visible: page.info.ok === true
            text: i18n("Access for people and groups is managed in the browser. Changing permissions there needs owner rights on the site; everyone can see who has access through Share in the online document.")
        }
        RowLayout {
            QQC2.Button {
                enabled: (page.info.permissionsUrl ?? "").length > 0
                icon.name: "system-users"
                text: i18n("Manage Permissions in Browser")
                onClicked: Qt.openUrlExternally(page.info.permissionsUrl)
            }
            QQC2.Button {
                enabled: (page.info.webUrl ?? "").length > 0
                icon.name: "internet-services"
                text: i18n("Open in Browser")
                onClicked: Qt.openUrlExternally(page.info.webUrl)
            }
        }
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
                icon.name: "dialog-close"
                text: i18n("Close")
                onClicked: applicationWindow().closePage()
            }
        }
    }
}
