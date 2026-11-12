// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.extras as PlasmaExtras

// The flyout, in the layout of the OneDrive one: header with the organisation and quick
// buttons, the recent activity, and a status line with the storage meter.
PlasmaExtras.Representation {
    id: full

    // The PlasmoidItem (or a stand-in in tests) providing status, activity and command()
    required property var app

    readonly property var status: app.status
    readonly property var mainRoot: app.personalRoot ?? (app.roots.length > 0 ? app.roots[0] : null)

    function plain(text) {
        return String(text ?? "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    function relativeTime(iso) {
        const seconds = Math.max(0, (Date.now() - new Date(iso).getTime()) / 1000);
        if (seconds < 60) {
            return i18n("Just now");
        }
        if (seconds < 3600) {
            return i18np("1 minute ago", "%1 minutes ago", Math.floor(seconds / 60));
        }
        if (seconds < 86400) {
            return i18np("1 hour ago", "%1 hours ago", Math.floor(seconds / 3600));
        }
        return Qt.formatDate(new Date(iso), Qt.locale().dateFormat(Locale.ShortFormat));
    }

    // Transfers of all locations together: {files, filesDone, bytes, bytesDone}
    readonly property var transfer: {
        let total = { files: 0, filesDone: 0, bytes: 0, bytesDone: 0 };
        for (const root of full.app.roots) {
            const t = root.transfer;
            if (t && t.filesDone < t.files) {
                total.files += t.files;
                total.filesDone += t.filesDone;
                total.bytes += t.bytes;
                total.bytesDone += t.bytesDone;
            }
        }
        return total;
    }

    function actionText(entry) {
        // Changed online by someone else
        if (entry.by && entry.action === "downloaded") {
            return i18n("Updated by %1 in %2", entry.by, entry.root);
        }
        switch (entry.action) {
        case "downloaded":
            return i18n("Downloaded to %1", entry.root);
        case "uploaded":
            return i18n("Uploaded to %1", entry.root);
        case "moved":
            return i18n("Moved in %1", entry.root);
        case "created":
            return i18n("Folder created in %1", entry.root);
        case "deleted":
            return i18n("Removed from this computer");
        case "deletedOnline":
            return i18n("Deleted from %1", entry.root);
        case "conflict":
            return i18n("Conflict: both versions kept");
        case "error":
            return entry.root;
        default:
            return entry.root;
        }
    }

    implicitWidth: Kirigami.Units.gridUnit * 24
    implicitHeight: Kirigami.Units.gridUnit * 28
    Layout.minimumWidth: Kirigami.Units.gridUnit * 20
    Layout.minimumHeight: Kirigami.Units.gridUnit * 20
    collapseMarginsHint: true

    header: PlasmaExtras.PlasmoidHeading {
        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.smallSpacing
                level: 3
                elide: Text.ElideRight
                text: (full.status.account?.organization ?? "") || i18n("Graph Sync")
            }
            PlasmaComponents.ToolButton {
                visible: full.mainRoot !== null
                icon.name: "document-open-folder"
                text: i18n("Open Folder")
                display: PlasmaComponents.AbstractButton.IconOnly
                PlasmaComponents.ToolTip.text: text
                PlasmaComponents.ToolTip.visible: hovered
                onClicked: Qt.openUrlExternally(full.app.fileUrl(full.mainRoot.localPath))
            }
            PlasmaComponents.ToolButton {
                visible: full.mainRoot !== null && (full.mainRoot.webUrl ?? "").length > 0
                icon.name: "internet-services"
                text: i18n("View Online")
                display: PlasmaComponents.AbstractButton.IconOnly
                PlasmaComponents.ToolTip.text: text
                PlasmaComponents.ToolTip.visible: hovered
                onClicked: Qt.openUrlExternally(full.mainRoot.webUrl)
            }
            PlasmaComponents.ToolButton {
                id: menuButton
                icon.name: "application-menu"
                text: i18n("Help & Settings")
                display: PlasmaComponents.AbstractButton.IconOnly
                checkable: true
                checked: mainMenu.visible
                PlasmaComponents.ToolTip.text: text
                PlasmaComponents.ToolTip.visible: hovered && !mainMenu.visible
                onClicked: mainMenu.popup(menuButton, 0, menuButton.height)
            }
        }
    }

    PlasmaComponents.Menu {
        id: mainMenu

        PlasmaComponents.MenuItem {
            icon.name: "configure"
            text: i18n("Settings")
            onTriggered: full.app.launch([])
        }
        PlasmaComponents.MenuItem {
            icon.name: "list-add"
            text: i18n("Add a Library…")
            enabled: full.status.signedIn === true
            onTriggered: full.app.launch(["--add"])
        }
        PlasmaComponents.MenuItem {
            icon.name: "list-add-user"
            text: i18n("Add Account…")
            enabled: full.app.accounts.some(a => a.signedIn)
            onTriggered: full.app.launch(["--add-account"])
        }
        PlasmaComponents.MenuSeparator {}
        PlasmaComponents.MenuItem {
            visible: full.status.paused === true && full.status.pausedByUser === true
            icon.name: "media-playback-start"
            text: i18n("Resume Syncing")
            onTriggered: full.app.command({ command: "resume" })
        }
        PlasmaComponents.MenuItem {
            visible: full.status.pausedByUser !== true
            icon.name: "media-playback-pause"
            text: i18n("Pause for 2 Hours")
            onTriggered: full.app.command({ command: "pause", minutes: 120 })
        }
        PlasmaComponents.MenuItem {
            visible: full.status.pausedByUser !== true
            icon.name: "media-playback-pause"
            text: i18n("Pause for 8 Hours")
            onTriggered: full.app.command({ command: "pause", minutes: 480 })
        }
        PlasmaComponents.MenuItem {
            visible: full.status.pausedByUser !== true
            icon.name: "media-playback-pause"
            text: i18n("Pause for 24 Hours")
            onTriggered: full.app.command({ command: "pause", minutes: 1440 })
        }
        PlasmaComponents.MenuSeparator {}
        PlasmaComponents.MenuItem {
            icon.name: "view-refresh"
            text: i18n("Sync Now")
            enabled: full.app.daemonRunning
            onTriggered: full.app.command({ command: "syncNow" })
        }
        PlasmaComponents.MenuItem {
            icon.name: "application-exit"
            text: i18n("Quit Graph Sync")
            enabled: full.app.daemonRunning
            onTriggered: full.app.command({ command: "quit" })
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.smallSpacing

        Repeater {
            model: full.app.signedOutAccounts
            delegate: Kirigami.InlineMessage {
                id: signedOutMessage
                required property var modelData
                Layout.fillWidth: true
                visible: true
                type: Kirigami.MessageType.Warning
                // Names are text, never markup (the message shows rich text)
                text: modelData.organization ? i18nc("%1 is the account, %2 the organization", "%1 (%2) is signed out, so its files are not syncing.", full.plain(modelData.userPrincipalName), full.plain(modelData.organization))
                                             : i18n("%1 is signed out, so its files are not syncing.", full.plain(modelData.userPrincipalName))
                actions: Kirigami.Action {
                    icon.name: "im-user-online"
                    text: i18n("Sign In")
                    // The delegate by id: an Action does not see the delegate's properties unqualified
                    onTriggered: full.app.signIn(signedOutMessage.modelData.id)
                }
            }
        }

        // More than one account: how each one is doing
        Repeater {
            model: full.app.syncAccounts.length > 1 ? full.app.syncAccounts.filter(a => a.signedIn) : []
            delegate: RowLayout {
                id: accountRow
                required property var modelData
                readonly property var own: full.app.roots.filter(r => r.accountId === modelData.id)
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Icon {
                    source: accountRow.own.some(r => r.state === "error" || r.state === "halted") ? "data-warning"
                        : (accountRow.own.some(r => r.state === "syncing") ? "view-refresh" : "data-success")
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                }
                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    font: Kirigami.Theme.smallFont
                    textFormat: Text.PlainText
                    text: accountRow.modelData.organization ? i18nc("%1 is the account, %2 the organization", "%1 (%2)", accountRow.modelData.userPrincipalName, accountRow.modelData.organization)
                                                            : accountRow.modelData.userPrincipalName
                }
                PlasmaComponents.Label {
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                    text: accountRow.own.some(r => r.state === "error" || r.state === "halted") ? i18n("Problem")
                        : (accountRow.own.some(r => r.state === "syncing") ? i18n("Syncing…") : i18n("Up to date"))
                }
            }
        }

        Repeater {
            model: full.app.bigDeletes
            delegate: Kirigami.InlineMessage {
                id: bigDeleteMessage
                required property var modelData
                Layout.fillWidth: true
                visible: true
                type: Kirigami.MessageType.Warning
                text: i18n("%1 of %2 files in %3 are gone from this computer. Nothing has been deleted online yet.", modelData.pendingBigDelete.missing, modelData.pendingBigDelete.known, modelData.name)
                actions: [
                    Kirigami.Action {
                        icon.name: "edit-undo"
                        text: i18n("Restore Files")
                        onTriggered: full.app.command({ command: "resolveBigDelete", id: bigDeleteMessage.modelData.id, action: "restore" })
                    },
                    Kirigami.Action {
                        icon.name: "edit-delete"
                        text: i18n("Delete Online Too")
                        onTriggered: full.app.command({ command: "resolveBigDelete", id: bigDeleteMessage.modelData.id, action: "delete" })
                    }
                ]
            }
        }

        PlasmaComponents.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            contentItem: ListView {
                id: activityList
                model: full.app.activity
                clip: true
                spacing: 0
                highlight: PlasmaExtras.Highlight {}
                highlightMoveDuration: 0
                currentIndex: -1

                PlasmaExtras.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    visible: activityList.count === 0
                    iconName: full.app.syncState === "signedOut" ? "im-user-offline" : (full.app.syncState === "offline" ? "network-disconnect" : "folder-cloud")
                    text: {
                        switch (full.app.syncState) {
                        case "offline":
                            return i18n("The sync service is not running");
                        case "signedOut":
                            return i18n("Sign in to start syncing");
                        default:
                            return full.app.roots.length === 0 ? i18n("Nothing is synced yet") : i18n("You're up to date");
                        }
                    }
                    explanation: full.app.syncState === "idle" && full.app.roots.length > 0 ? i18n("Recently synced files appear here.") : ""
                    helpfulAction: Kirigami.Action {
                        // The placeholder shows its button when the action is enabled; visible is ignored
                        enabled: full.app.syncState === "signedOut" || (full.app.syncState === "idle" && full.app.roots.length === 0)
                        icon.name: full.app.syncState === "signedOut" ? "im-user-online" : "folder-sync"
                        text: full.app.syncState === "signedOut" ? i18n("Sign In") : i18n("Set Up")
                        onTriggered: {
                            if (full.app.syncState === "signedOut") {
                                full.app.launch(["--sign-in"]);
                            } else {
                                // Signed in but nothing synced: set up, or add a library after setup
                                full.app.launch(full.status.setupDone ? ["--add"] : ["--setup"]);
                            }
                        }
                    }
                }

                delegate: PlasmaComponents.ItemDelegate {
                    id: entryDelegate
                    required property var modelData
                    required property int index
                    readonly property bool hasFile: (modelData.path ?? "").length > 0 && modelData.action !== "deletedOnline" && modelData.action !== "deleted"

                    width: ListView.view.width
                    onClicked: {
                        if (hasFile) {
                            Qt.openUrlExternally(full.app.fileUrl(modelData.path));
                        }
                    }
                    onHoveredChanged: {
                        if (hovered) {
                            ListView.view.currentIndex = index;
                        }
                    }

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        Kirigami.Icon {
                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                            source: entryDelegate.modelData.action === "error" ? "data-warning" : (entryDelegate.modelData.icon || "text-x-generic")
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            PlasmaComponents.Label {
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                                maximumLineCount: 2
                                wrapMode: entryDelegate.modelData.name ? Text.NoWrap : Text.Wrap
                                text: entryDelegate.modelData.name || entryDelegate.modelData.message
                            }
                            PlasmaComponents.Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                text: full.actionText(entryDelegate.modelData) + " · " + full.relativeTime(entryDelegate.modelData.time)
                            }
                        }
                        PlasmaComponents.ToolButton {
                            visible: entryDelegate.hasFile && (entryDelegate.hovered || entryMenu.visible)
                            icon.name: "overflow-menu"
                            text: i18n("More Actions")
                            display: PlasmaComponents.AbstractButton.IconOnly
                            onClicked: entryMenu.popup(this, 0, height)
                        }
                    }

                    PlasmaComponents.Menu {
                        id: entryMenu
                        PlasmaComponents.MenuItem {
                            icon.name: "document-open"
                            text: i18n("Open")
                            onTriggered: Qt.openUrlExternally(full.app.fileUrl(entryDelegate.modelData.path))
                        }
                        PlasmaComponents.MenuItem {
                            icon.name: "document-open-folder"
                            text: i18n("Show in Folder")
                            onTriggered: full.app.command({ command: "showInFolder", path: entryDelegate.modelData.path })
                        }
                        PlasmaComponents.MenuItem {
                            icon.name: "internet-services"
                            text: i18n("View Online")
                            onTriggered: full.app.launch(["--view-online", entryDelegate.modelData.path])
                        }
                        PlasmaComponents.MenuSeparator {}
                        PlasmaComponents.MenuItem {
                            icon.name: "emblem-shared-symbolic"
                            text: i18n("Share…")
                            onTriggered: full.app.launch(["--share", entryDelegate.modelData.path])
                        }
                        PlasmaComponents.MenuItem {
                            icon.name: "edit-copy"
                            text: i18n("Copy Link")
                            onTriggered: full.app.launch(["--copy-link", entryDelegate.modelData.path])
                        }
                        PlasmaComponents.MenuItem {
                            icon.name: "view-history"
                            text: i18n("Version History…")
                            onTriggered: full.app.launch(["--versions", entryDelegate.modelData.path])
                        }
                    }
                }
            }
        }
    }

    footer: PlasmaExtras.PlasmoidHeading {
        position: PlasmaComponents.ToolBar.Footer
        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Icon {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    source: full.app.syncState === "error" ? "data-warning" : (full.app.syncState === "paused" ? "media-playback-pause" : (full.app.syncState === "syncing" ? "view-refresh" : "data-success"))
                }
                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: full.app.statusText
                }
                PlasmaComponents.ToolButton {
                    visible: full.app.problemRoots.length > 0
                    checkable: true
                    text: checked ? i18n("Hide Issues") : i18n("View Issues")
                    onToggled: issues.visible = checked
                }
            }

            // Like the Windows flyout: how far the current transfers are
            ColumnLayout {
                Layout.fillWidth: true
                visible: full.transfer.files > 0
                spacing: 0
                PlasmaComponents.ProgressBar {
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, full.transfer.bytes)
                    value: full.transfer.bytesDone
                    indeterminate: full.transfer.bytes === 0
                }
                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                    text: i18np("%2 of 1 file", "%2 of %1 files", full.transfer.files, full.transfer.filesDone) + "  ·  "
                        + i18n("%1 of %2", Qt.locale().formattedDataSize(full.transfer.bytesDone), Qt.locale().formattedDataSize(full.transfer.bytes))
                }
            }

            ColumnLayout {
                id: issues
                Layout.fillWidth: true
                visible: false
                Repeater {
                    model: full.app.problemRoots
                    delegate: PlasmaComponents.Label {
                        required property var modelData
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font: Kirigami.Theme.smallFont
                        text: modelData.name + ": " + (modelData.haltReason ?? (modelData.lastErrors ?? []).concat(modelData.persistentErrors ?? []).join("\n"))
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: (full.app.personalRoot?.quotaTotal ?? 0) > 0
                spacing: Kirigami.Units.smallSpacing
                PlasmaComponents.ProgressBar {
                    Layout.fillWidth: true
                    from: 0
                    to: full.app.personalRoot?.quotaTotal ?? 1
                    value: full.app.personalRoot?.quotaUsed ?? 0
                }
                PlasmaComponents.Label {
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                    text: i18n("%1 of %2 used", Qt.locale().formattedDataSize(full.app.personalRoot?.quotaUsed ?? 0), Qt.locale().formattedDataSize(full.app.personalRoot?.quotaTotal ?? 0))
                }
            }
        }
    }
}
