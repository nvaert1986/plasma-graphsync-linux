// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The settings, laid out like OneDrive's Settings on Windows: navigation on the left, with
// "Locations that are syncing" showing what is synced and in what state. Shared by the app's
// window (SettingsPage.qml) and the page in System Settings (src/kcm/ui/main.qml).
Item {
    id: page

    // Backend (app) or the same Backend exposed by the System Settings module
    required property var api
    // Pages the view cannot show itself: "add", "chooseFolders" (argument = root id), "signIn".
    // The app opens them in its window; System Settings starts the app for them.
    signal openRequested(string mode, string argument)

    readonly property var status: page.api.status
    readonly property var roots: status.roots ?? []
    readonly property var accounts: status.accounts ?? []
    // Accounts this computer syncs with (or is signed in to); the others in graphaccountsrc were
    // added by another app of the suite
    readonly property var syncAccounts: accounts.filter(a => a.signedIn || a.locations > 0 || a.current)
    readonly property var otherAccounts: accounts.filter(a => !a.signedIn && a.locations === 0 && !a.current)
    // With more than one account, locations are listed per account under its name
    readonly property bool grouped: syncAccounts.length > 1
    readonly property var rootEntries: {
        if (!grouped) {
            return roots.map(r => ({ root: r, header: null }));
        }
        let entries = [];
        for (const account of syncAccounts) {
            const own = roots.filter(r => r.accountId === account.id);
            if (own.length === 0) {
                entries.push({ root: null, header: account });
            }
            own.forEach((r, i) => entries.push({ root: r, header: i === 0 ? account : null }));
        }
        // Set up while signed out: the next sign-in claims them
        const orphans = roots.filter(r => !accounts.some(a => a.id === r.accountId));
        orphans.forEach((r, i) => entries.push({ root: r, header: i === 0 ? { id: "", userPrincipalName: i18n("Waiting for an account"), organization: "", signedIn: false } : null }));
        return entries;
    }
    property int section: 0
    property var rootToRemove: null
    property var accountToChange: null

    // Names from an organisation's directory (its name set by its administrator) are text, never
    // markup: for texts that cannot be set to plain text
    function plain(text) {
        return String(text ?? "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    function accountTitle(account) {
        if (!account) {
            return "";
        }
        return account.organization ? i18nc("%1 is the account, %2 the organization", "%1 (%2)", account.userPrincipalName, account.organization) : account.userPrincipalName;
    }

    function relativeTime(iso) {
        if (!iso) {
            return "";
        }
        const seconds = Math.max(0, (Date.now() - new Date(iso).getTime()) / 1000);
        if (seconds < 60) {
            return i18n("just now");
        }
        if (seconds < 3600) {
            return i18np("1 minute ago", "%1 minutes ago", Math.floor(seconds / 60));
        }
        if (seconds < 86400) {
            return i18np("1 hour ago", "%1 hours ago", Math.floor(seconds / 3600));
        }
        return Qt.formatDateTime(new Date(iso), Qt.locale().dateTimeFormat(Locale.ShortFormat));
    }

    function overallText() {
        if (!page.api.daemonRunning) {
            return i18n("The sync service is not running");
        }
        if (status.signedIn === false && !accounts.some(a => a.signedIn)) {
            return i18n("Not signed in");
        }
        if (status.paused) {
            return status.pausedUntil ? i18n("Syncing is paused until %1", Qt.formatTime(new Date(status.pausedUntil), Qt.locale().timeFormat(Locale.ShortFormat))) : i18n("Syncing is paused");
        }
        if (roots.length > 0 && roots.every(r => r.state === "offline")) {
            return i18n("Offline: syncing continues when the network is back");
        }
        if (roots.some(r => r.state === "error" || r.state === "halted" || r.state === "signedOut")) {
            return i18n("Some locations have a problem");
        }
        if (roots.some(r => r.state === "syncing")) {
            return i18n("Syncing…");
        }
        return roots.length > 0 ? i18n("Your files are synced") : i18n("Nothing is synced yet");
    }

    function rootState(root) {
        switch (root.state) {
        case "syncing":
            return { icon: "view-refresh", text: i18n("Syncing…"), problem: false };
        case "offline":
            return { icon: "network-disconnect", text: i18n("Offline: syncing continues when the network is back"), problem: false };
        case "paused":
            return { icon: "media-playback-pause", text: i18n("Paused"), problem: false };
        case "error":
            return { icon: "data-warning", text: root.pendingBigDelete ? i18n("Many files were removed here; waiting for your decision") : (root.lastErrors ?? [""])[0], problem: true };
        case "halted":
            return { icon: "data-error", text: root.haltReason ?? i18n("Stopped"), problem: true };
        case "signedOut":
            return { icon: "im-user-offline", text: i18n("Signed out: sign in to sync this location again"), problem: true };
        default:
            return { icon: "data-success", text: root.lastSync ? i18n("Up to date, last synced %1", relativeTime(root.lastSync)) : i18n("Waiting for the first sync"), problem: false };
        }
    }

    Kirigami.PromptDialog {
        id: removeDialog
        title: i18n("Stop Syncing?")
        subtitle: page.rootToRemove ? i18n("%1 will no longer be synced. The files stay online, and the local copies in %2 are kept as ordinary files.", page.rootToRemove.name, page.rootToRemove.localPath) : ""
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                icon.name: "edit-delete-remove"
                text: i18n("Stop Syncing")
                onTriggered: {
                    page.api.removeRoot(page.rootToRemove.id);
                    removeDialog.close();
                }
            },
            Kirigami.Action {
                icon.name: "dialog-cancel"
                text: i18n("Cancel")
                onTriggered: removeDialog.close()
            }
        ]
    }

    Kirigami.PromptDialog {
        id: signOutDialog
        title: i18n("Sign Out of This Computer?")
        subtitle: page.accountToChange && page.syncAccounts.length > 1
            ? i18n("The locations of %1 stop syncing until you sign in again. Your local files are kept.", page.accountToChange.userPrincipalName)
            : i18n("Syncing stops until you sign in again. Your local files are kept.")
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                icon.name: "system-log-out"
                text: i18n("Sign Out")
                onTriggered: {
                    page.api.signOutAccount(page.accountToChange?.id ?? page.status.account?.id ?? "");
                    signOutDialog.close();
                }
            },
            Kirigami.Action {
                icon.name: "dialog-cancel"
                text: i18n("Cancel")
                onTriggered: signOutDialog.close()
            }
        ]
    }

    Kirigami.PromptDialog {
        id: removeAccountDialog
        title: i18n("Remove Account?")
        subtitle: page.accountToChange ? i18n("%1 is signed out and its locations stop syncing. The files stay online, and the local copies are kept as ordinary files.", page.accountToChange.userPrincipalName) : ""
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                icon.name: "list-remove-user"
                text: i18n("Remove Account")
                onTriggered: {
                    page.api.removeAccount(page.accountToChange.id);
                    removeAccountDialog.close();
                }
            },
            Kirigami.Action {
                icon.name: "dialog-cancel"
                text: i18n("Cancel")
                onTriggered: removeAccountDialog.close()
            }
        ]
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Navigation
        QQC2.ScrollView {
            Layout.preferredWidth: Kirigami.Units.gridUnit * 11
            Layout.fillHeight: true
            QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

            ListView {
                id: navigation
                model: [
                    { icon: "folder-sync", text: i18n("Sync") },
                    { icon: "im-user", text: i18n("Accounts") },
                    { icon: "preferences-desktop-notification", text: i18n("Notifications") },
                    { icon: "configure", text: i18n("Advanced") },
                    { icon: "help-about", text: i18n("About") }
                ]
                currentIndex: page.section
                delegate: QQC2.ItemDelegate {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    icon.name: modelData.icon
                    text: modelData.text
                    highlighted: ListView.isCurrentItem
                    onClicked: page.section = index
                }
            }
        }

        Kirigami.Separator {
            Layout.fillHeight: true
        }

        QQC2.ScrollView {
            id: contentScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

            // The margins are part of the scrolled height, so the last card ends above the bottom
            // edge; the height follows the shown section, so short sections do not scroll
            Item {
                implicitWidth: contentScroll.availableWidth
                implicitHeight: (sections.children[sections.currentIndex]?.implicitHeight ?? 0) + Kirigami.Units.largeSpacing * 4

                StackLayout {
                    id: sections
                    x: Kirigami.Units.largeSpacing * 2
                    y: Kirigami.Units.largeSpacing * 2
                    width: parent.implicitWidth - Kirigami.Units.largeSpacing * 4
                    height: parent.implicitHeight - Kirigami.Units.largeSpacing * 4
                    currentIndex: page.section

                    // Sync
                    ColumnLayout {
                        spacing: Kirigami.Units.largeSpacing

                        Kirigami.InlineMessage {
                            Layout.fillWidth: true
                            visible: !page.api.daemonRunning
                            type: Kirigami.MessageType.Warning
                            text: i18n("The sync service is not running, so nothing is being synced.")
                            actions: Kirigami.Action {
                                icon.name: "media-playback-start"
                                text: i18n("Start")
                                onTriggered: page.api.startDaemon()
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.largeSpacing
                            Kirigami.Icon {
                                source: page.status.paused ? "media-playback-pause" : (page.roots.some(r => r.state === "error" || r.state === "halted") ? "data-warning" : "io.github.nvaert1986.graphsync")
                                implicitWidth: Kirigami.Units.iconSizes.large
                                implicitHeight: Kirigami.Units.iconSizes.large
                            }
                            Kirigami.Heading {
                                Layout.fillWidth: true
                                level: 2
                                wrapMode: Text.Wrap
                                text: page.overallText()
                            }
                            QQC2.Button {
                                visible: page.status.pausedByUser === true
                                icon.name: "media-playback-start"
                                text: i18n("Resume Syncing")
                                onClicked: page.api.resume()
                            }
                            QQC2.Button {
                                visible: page.api.daemonRunning && page.status.pausedByUser !== true
                                icon.name: "media-playback-pause"
                                text: i18n("Pause Syncing")
                                onClicked: pauseMenu.popup()
                                QQC2.Menu {
                                    id: pauseMenu
                                    QQC2.MenuItem {
                                        text: i18n("2 Hours")
                                        onTriggered: page.api.pause(120)
                                    }
                                    QQC2.MenuItem {
                                        text: i18n("8 Hours")
                                        onTriggered: page.api.pause(480)
                                    }
                                    QQC2.MenuItem {
                                        text: i18n("24 Hours")
                                        onTriggered: page.api.pause(1440)
                                    }
                                }
                            }
                            QQC2.Button {
                                visible: page.api.daemonRunning && !page.status.paused
                                icon.name: "view-refresh"
                                text: i18n("Sync Now")
                                onClicked: page.api.syncNow()
                            }
                        }

                        QQC2.CheckBox {
                            text: i18n("Pause syncing on metered connections (mobile hotspots)")
                            checked: page.api.pauseOnMeteredNetwork
                            onToggled: page.api.pauseOnMeteredNetwork = checked
                        }
                        RowLayout {
                            QQC2.Label {
                                text: i18n("Copy Link gives people:")
                            }
                            QQC2.ComboBox {
                                textRole: "text"
                                valueRole: "value"
                                model: [
                                    { text: i18n("View access"), value: "view" },
                                    { text: i18n("Edit access"), value: "edit" }
                                ]
                                currentIndex: page.api.defaultLinkType === "edit" ? 1 : 0
                                onActivated: page.api.defaultLinkType = currentValue
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: Kirigami.Units.largeSpacing
                            Kirigami.Heading {
                                Layout.fillWidth: true
                                level: 3
                                text: i18n("Locations that are syncing")
                            }
                            QQC2.Button {
                                // Grouped: every account has its own button
                                visible: page.roots.length > 0 && !page.grouped
                                enabled: page.api.signedIn
                                icon.name: "list-add"
                                text: i18n("Add a Library…")
                                onClicked: page.openRequested("add", "")
                            }
                        }

                        Kirigami.PlaceholderMessage {
                            Layout.fillWidth: true
                            visible: page.roots.length === 0
                            icon.name: "folder-cloud"
                            text: i18n("Nothing is synced yet")
                            explanation: i18n("Use the Sync button in SharePoint, or add a library here.")
                            helpfulAction: Kirigami.Action {
                                enabled: page.api.signedIn
                                icon.name: "list-add"
                                text: i18n("Add a Library…")
                                onTriggered: page.openRequested("add", "")
                            }
                        }

                        Repeater {
                            model: page.rootEntries
                            delegate: ColumnLayout {
                                id: entry
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing

                                // The account above its locations
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: Kirigami.Units.largeSpacing
                                    visible: entry.modelData.header !== null
                                    Kirigami.Icon {
                                        source: entry.modelData.header?.signedIn ? "im-user" : "im-user-offline"
                                        implicitWidth: Kirigami.Units.iconSizes.smallMedium
                                        implicitHeight: Kirigami.Units.iconSizes.smallMedium
                                    }
                                    QQC2.Label {
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        font.bold: true
                                        textFormat: Text.PlainText
                                        text: page.accountTitle(entry.modelData.header)
                                    }
                                    QQC2.Button {
                                        visible: entry.modelData.header?.signedIn === true
                                        flat: true
                                        icon.name: "list-add"
                                        text: i18n("Add a Library…")
                                        onClicked: {
                                            page.api.setCurrentAccount(entry.modelData.header.id);
                                            page.openRequested("add", "");
                                        }
                                    }
                                    QQC2.Button {
                                        visible: entry.modelData.header !== null && entry.modelData.header.signedIn === false && entry.modelData.header.id !== ""
                                        icon.name: "im-user-online"
                                        text: i18n("Sign In…")
                                        onClicked: {
                                            page.api.setCurrentAccount(entry.modelData.header.id);
                                            page.openRequested("signIn", "");
                                        }
                                    }
                                }
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    visible: entry.modelData.header !== null && entry.modelData.root === null
                                    opacity: 0.7
                                    text: i18n("No locations of this account are synced.")
                                }
                                Kirigami.AbstractCard {
                                    id: card
                                    readonly property var modelData: entry.modelData.root ?? ({})
                                    readonly property var rootStatus: page.rootState(modelData)
                                    // "Add shortcut to My files": managed online, so no Choose Folders or Stop Syncing
                                    readonly property bool isShortcut: modelData.kind === "shortcut"
                                    Layout.fillWidth: true
                                    visible: entry.modelData.root !== null

                                    contentItem: RowLayout {
                                        spacing: Kirigami.Units.largeSpacing
                                        Kirigami.Icon {
                                            Layout.alignment: Qt.AlignTop
                                            source: card.modelData.kind === "library" || card.modelData.kind === "folder" ? "folder-network" : "folder-cloud"
                                            implicitWidth: Kirigami.Units.iconSizes.large
                                            implicitHeight: Kirigami.Units.iconSizes.large
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: Kirigami.Units.smallSpacing
                                            Kirigami.Heading {
                                                Layout.fillWidth: true
                                                level: 4
                                                elide: Text.ElideRight
                                                text: card.modelData.name
                                            }
                                            QQC2.Label {
                                                Layout.fillWidth: true
                                                visible: card.isShortcut
                                                elide: Text.ElideRight
                                                text: i18nc("%1 is a sync location such as OneDrive - Contoso", "Shortcut in %1", page.roots.find(r => r.id === card.modelData.parentRootId)?.name ?? "")
                                            }
                                            QQC2.Label {
                                                Layout.fillWidth: true
                                                elide: Text.ElideMiddle
                                                opacity: 0.7
                                                text: card.modelData.localPath
                                            }
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: Kirigami.Units.smallSpacing
                                                Kirigami.Icon {
                                                    source: card.rootStatus.icon
                                                    implicitWidth: Kirigami.Units.iconSizes.small
                                                    implicitHeight: Kirigami.Units.iconSizes.small
                                                }
                                                QQC2.Label {
                                                    Layout.fillWidth: true
                                                    wrapMode: Text.Wrap
                                                    color: card.rootStatus.problem ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                                                    text: card.rootStatus.text
                                                }
                                            }
                                            QQC2.Label {
                                                Layout.fillWidth: true
                                                // Not counted while its account is signed out
                                                visible: card.modelData.state !== "signedOut"
                                                wrapMode: Text.Wrap
                                                opacity: 0.7
                                                text: {
                                                    const parts = [i18np("1 item", "%1 items", card.modelData.items ?? 0)];
                                                    const excluded = (card.modelData.excludedPaths ?? []).length;
                                                    if (excluded > 0) {
                                                        parts.push(i18np("1 folder not synced", "%1 folders not synced", excluded));
                                                    }
                                                    const onlineOnly = card.modelData.onlineOnly ?? 0;
                                                    if (onlineOnly > 0) {
                                                        parts.push(i18np("1 online only", "%1 online only", onlineOnly));
                                                    }
                                                    return parts.join(" · ");
                                                }
                                            }
                                            QQC2.CheckBox {
                                                visible: !card.isShortcut
                                                text: i18n("Files On-Demand: download files only when you open them")
                                                checked: card.modelData.filesOnDemand === true
                                                onToggled: page.api.setFilesOnDemand(card.modelData.id, checked)
                                                QQC2.ToolTip.visible: hovered
                                                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                                QQC2.ToolTip.text: i18n("New files from online appear as placeholders (name.cloud) and are downloaded when you open them. Right-click files or folders in Dolphin to keep them on this computer or to free up space. Switching this off downloads everything again.")
                                            }
                                            QQC2.Label {
                                                Layout.fillWidth: true
                                                visible: card.isShortcut
                                                wrapMode: Text.Wrap
                                                opacity: 0.7
                                                text: i18n("To stop syncing it, remove the shortcut in OneDrive online, or delete this folder here.")
                                            }
                                            Flow {
                                                Layout.fillWidth: true
                                                Layout.topMargin: Kirigami.Units.smallSpacing
                                                spacing: Kirigami.Units.smallSpacing
                                                QQC2.Button {
                                                    icon.name: "document-open-folder"
                                                    text: i18n("Open Folder")
                                                    onClicked: Qt.openUrlExternally(page.api.fileUrl(card.modelData.localPath))
                                                }
                                                QQC2.Button {
                                                    visible: (card.modelData.webUrl ?? "").length > 0
                                                    icon.name: "internet-services"
                                                    text: i18n("View Online")
                                                    onClicked: Qt.openUrlExternally(card.modelData.webUrl)
                                                }
                                                QQC2.Button {
                                                    visible: !card.isShortcut
                                                    icon.name: "folder-open-recent"
                                                    text: i18n("Choose Folders…")
                                                    enabled: card.modelData.state !== "signedOut"
                                                    // Folders are listed with the location's own account
                                                    onClicked: page.openRequested("chooseFolders", card.modelData.id)
                                                }
                                                QQC2.Button {
                                                    visible: !card.isShortcut
                                                    icon.name: "edit-delete-remove"
                                                    text: i18n("Stop Syncing")
                                                    onClicked: {
                                                        page.rootToRemove = card.modelData;
                                                        removeDialog.open();
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Item {
                            Layout.fillHeight: true
                        }
                    }

                    // Accounts
                    ColumnLayout {
                        spacing: Kirigami.Units.largeSpacing

                        Kirigami.PlaceholderMessage {
                            Layout.fillWidth: true
                            visible: page.syncAccounts.length === 0
                            icon.name: "im-user-offline"
                            text: i18n("Not signed in")
                            helpfulAction: Kirigami.Action {
                                icon.name: "im-user-online"
                                text: i18n("Sign In…")
                                onTriggered: page.openRequested("signIn", "")
                            }
                        }

                        Repeater {
                            model: page.syncAccounts
                            delegate: Kirigami.AbstractCard {
                                id: accountCard
                                required property var modelData
                                readonly property var personal: page.roots.find(r => r.kind === "personal" && r.accountId === modelData.id) ?? null
                                Layout.fillWidth: true

                                contentItem: RowLayout {
                                    spacing: Kirigami.Units.largeSpacing
                                    Kirigami.Icon {
                                        Layout.alignment: Qt.AlignTop
                                        source: accountCard.modelData.signedIn ? "im-user" : "im-user-offline"
                                        implicitWidth: Kirigami.Units.iconSizes.large
                                        implicitHeight: Kirigami.Units.iconSizes.large
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: Kirigami.Units.smallSpacing
                                        Kirigami.Heading {
                                            Layout.fillWidth: true
                                            level: 4
                                            elide: Text.ElideRight
                                            textFormat: Text.PlainText
                                            text: accountCard.modelData.displayName || accountCard.modelData.userPrincipalName
                                        }
                                        QQC2.Label {
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                            textFormat: Text.PlainText
                                            text: accountCard.modelData.organization ? i18nc("%1 is the account, %2 the organization", "%1 · %2", accountCard.modelData.userPrincipalName, accountCard.modelData.organization) : accountCard.modelData.userPrincipalName
                                        }
                                        QQC2.Label {
                                            Layout.fillWidth: true
                                            wrapMode: Text.Wrap
                                            color: !accountCard.modelData.signedIn && accountCard.modelData.locations > 0 ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                                            opacity: color === Kirigami.Theme.negativeTextColor ? 1 : 0.7
                                            text: accountCard.modelData.signedIn
                                                ? i18np("Signed in · 1 location", "Signed in · %1 locations", accountCard.modelData.locations)
                                                : (accountCard.modelData.locations > 0 ? i18np("Signed out: its location is not syncing", "Signed out: its %1 locations are not syncing", accountCard.modelData.locations) : i18n("Signed out"))
                                        }
                                        QQC2.Label {
                                            Layout.fillWidth: true
                                            visible: accountCard.modelData.current && page.syncAccounts.length > 1
                                            wrapMode: Text.Wrap
                                            opacity: 0.7
                                            text: i18n("Used for new locations and for the Sync button in SharePoint.")
                                        }
                                        RowLayout {
                                            visible: (accountCard.personal?.quotaTotal ?? 0) > 0
                                            QQC2.ProgressBar {
                                                Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                                                from: 0
                                                to: accountCard.personal?.quotaTotal ?? 1
                                                value: accountCard.personal?.quotaUsed ?? 0
                                            }
                                            QQC2.Label {
                                                opacity: 0.7
                                                text: i18n("%1 of %2 used", Qt.locale().formattedDataSize(accountCard.personal?.quotaUsed ?? 0), Qt.locale().formattedDataSize(accountCard.personal?.quotaTotal ?? 0))
                                            }
                                        }
                                        QQC2.CheckBox {
                                            enabled: accountCard.modelData.signedIn && page.api.daemonRunning
                                            text: i18nc("%1 is the organization", "Sync OneDrive - %1", page.plain(accountCard.modelData.organization))
                                            checked: accountCard.personal !== null
                                            onToggled: {
                                                if (checked) {
                                                    // The OneDrive of this account; the current account stays
                                                    page.api.addPersonalFor(accountCard.modelData.id);
                                                } else {
                                                    page.rootToRemove = accountCard.personal;
                                                    removeDialog.open();
                                                }
                                                checked = Qt.binding(() => accountCard.personal !== null);
                                            }
                                        }
                                        Flow {
                                            Layout.fillWidth: true
                                            Layout.topMargin: Kirigami.Units.smallSpacing
                                            spacing: Kirigami.Units.smallSpacing
                                            QQC2.Button {
                                                visible: !accountCard.modelData.signedIn
                                                icon.name: "im-user-online"
                                                text: i18n("Sign In…")
                                                onClicked: {
                                                    page.api.setCurrentAccount(accountCard.modelData.id);
                                                    page.openRequested("signIn", "");
                                                }
                                            }
                                            QQC2.Button {
                                                visible: accountCard.modelData.signedIn && !accountCard.modelData.current
                                                icon.name: "checkmark"
                                                text: i18n("Use for New Locations")
                                                onClicked: page.api.setCurrentAccount(accountCard.modelData.id)
                                            }
                                            QQC2.Button {
                                                visible: accountCard.modelData.signedIn
                                                icon.name: "system-log-out"
                                                text: i18n("Sign Out…")
                                                onClicked: {
                                                    page.accountToChange = accountCard.modelData;
                                                    signOutDialog.open();
                                                }
                                            }
                                            QQC2.Button {
                                                visible: accountCard.modelData.locations > 0 && page.api.daemonRunning
                                                icon.name: "list-remove-user"
                                                text: i18n("Remove…")
                                                onClicked: {
                                                    page.accountToChange = accountCard.modelData;
                                                    removeAccountDialog.open();
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        QQC2.Button {
                            visible: page.syncAccounts.length > 0
                            icon.name: "list-add-user"
                            text: i18n("Add Account…")
                            onClicked: page.openRequested("addAccount", "")
                        }

                        // Accounts another app of the suite added (Graph Mail): one sign-in here
                        // syncs their files too
                        Kirigami.Heading {
                            Layout.topMargin: Kirigami.Units.largeSpacing
                            visible: page.otherAccounts.length > 0
                            level: 3
                            text: i18n("Other accounts on this computer")
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            visible: page.otherAccounts.length > 0
                            wrapMode: Text.Wrap
                            opacity: 0.7
                            text: i18n("Added by another app, such as Graph Mail. Sign in to sync their files too.")
                        }
                        Repeater {
                            model: page.otherAccounts
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                Kirigami.Icon {
                                    source: "im-user-offline"
                                    implicitWidth: Kirigami.Units.iconSizes.smallMedium
                                    implicitHeight: Kirigami.Units.iconSizes.smallMedium
                                }
                                QQC2.Label {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    textFormat: Text.PlainText
                                    text: page.accountTitle(modelData)
                                }
                                QQC2.Button {
                                    icon.name: "im-user-online"
                                    text: i18n("Sign In…")
                                    onClicked: {
                                        page.api.setCurrentAccount(modelData.id);
                                        page.openRequested("signIn", "");
                                    }
                                }
                            }
                        }

                        Item {
                            Layout.fillHeight: true
                        }
                    }

                    // Notifications
                    ColumnLayout {
                        spacing: Kirigami.Units.largeSpacing
                        QQC2.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: i18n("You are notified about sync conflicts, problems, newly synced libraries, many removed files and an expired sign-in. Choose which of these show a popup, play a sound or are only logged.")
                        }
                        QQC2.Button {
                            icon.name: "preferences-desktop-notification"
                            text: i18n("Configure Notifications…")
                            onClicked: page.api.openNotificationSettings()
                        }
                        Item {
                            Layout.fillHeight: true
                        }
                    }

                    // Advanced: network, exclusions, disk space, app registration, troubleshooting
                    ColumnLayout {
                        spacing: Kirigami.Units.largeSpacing

                        Kirigami.FormLayout {
                            Layout.fillWidth: true

                            Kirigami.Separator {
                                Kirigami.FormData.isSection: true
                                Kirigami.FormData.label: i18n("Network")
                            }
                            RowLayout {
                                Kirigami.FormData.label: i18n("Download rate:")
                                QQC2.CheckBox {
                                    id: limitDownload
                                    text: i18n("Limit to")
                                    checked: page.api.downloadLimit > 0
                                    onToggled: page.api.downloadLimit = checked ? downloadRate.value : 0
                                }
                                QQC2.SpinBox {
                                    id: downloadRate
                                    enabled: limitDownload.checked
                                    from: 10
                                    to: 1000000
                                    stepSize: 100
                                    editable: true
                                    value: page.api.downloadLimit > 0 ? page.api.downloadLimit : 1000
                                    onValueModified: page.api.downloadLimit = value
                                }
                                QQC2.Label {
                                    text: i18nc("kilobytes per second", "KB/s")
                                }
                            }
                            RowLayout {
                                Kirigami.FormData.label: i18n("Upload rate:")
                                QQC2.CheckBox {
                                    id: limitUpload
                                    text: i18n("Limit to")
                                    checked: page.api.uploadLimit > 0
                                    onToggled: page.api.uploadLimit = checked ? uploadRate.value : 0
                                }
                                QQC2.SpinBox {
                                    id: uploadRate
                                    enabled: limitUpload.checked
                                    from: 10
                                    to: 1000000
                                    stepSize: 100
                                    editable: true
                                    value: page.api.uploadLimit > 0 ? page.api.uploadLimit : 1000
                                    onValueModified: page.api.uploadLimit = value
                                }
                                QQC2.Label {
                                    text: i18nc("kilobytes per second", "KB/s")
                                }
                            }

                            Kirigami.Separator {
                                Kirigami.FormData.isSection: true
                                Kirigami.FormData.label: i18n("Files Not to Sync")
                            }
                            QQC2.TextArea {
                                Kirigami.FormData.label: i18n("Names:")
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 18
                                Layout.preferredHeight: Kirigami.Units.gridUnit * 5
                                placeholderText: i18n("One per line, for example:\n*.tmp\nnode_modules")
                                text: page.api.excludePatterns
                                onEditingFinished: page.api.excludePatterns = text
                            }
                            QQC2.CheckBox {
                                text: i18n("Hidden files and folders (names starting with a dot)")
                                checked: page.api.skipDotFiles
                                onToggled: page.api.skipDotFiles = checked
                            }
                            QQC2.Label {
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 22
                                wrapMode: Text.Wrap
                                opacity: 0.7
                                text: i18n("These stay on this computer and online as they are, but changes are no longer synced. To keep a whole folder on this computer only, put an empty file named .nosync in it.")
                            }

                            Kirigami.Separator {
                                Kirigami.FormData.isSection: true
                                Kirigami.FormData.label: i18n("Disk Space")
                            }
                            RowLayout {
                                Kirigami.FormData.label: i18n("Always keep free:")
                                QQC2.SpinBox {
                                    from: 0
                                    to: 1000000
                                    stepSize: 512
                                    editable: true
                                    value: page.api.minimumFreeSpace
                                    onValueModified: page.api.minimumFreeSpace = value
                                }
                                QQC2.Label {
                                    text: i18nc("megabytes", "MB")
                                }
                            }
                        }

                        Kirigami.Heading {
                            Layout.fillWidth: true
                            Layout.topMargin: Kirigami.Units.largeSpacing
                            level: 3
                            text: i18n("App Registration")
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: i18n("The client ID of your organization's app registration and your organization (tenant). Change these only when your IT administrator asks you to; changing them signs you out of this computer.")
                        }
                        Kirigami.FormLayout {
                            Layout.fillWidth: true
                            QQC2.TextField {
                                id: regClient
                                Kirigami.FormData.label: i18n("Client ID:")
                                implicitWidth: Kirigami.Units.gridUnit * 20
                                text: page.api.clientId
                            }
                            QQC2.TextField {
                                id: regTenant
                                Kirigami.FormData.label: i18n("Tenant ID or work e-mail address:")
                                implicitWidth: Kirigami.Units.gridUnit * 20
                                text: page.api.tenantId
                            }
                            QQC2.Button {
                                enabled: !page.api.busy && regClient.text.trim().length > 0
                                         && (regClient.text.trim() !== page.api.clientId || regTenant.text.trim() !== page.api.tenantId)
                                icon.name: "document-save"
                                text: page.api.busy ? i18n("Looking up your organization…") : i18n("Save")
                                onClicked: {
                                    const ok = page.api.saveAppRegistration(regClient.text, regTenant.text);
                                    regResult.text = ok ? i18n("Saved. Sign in again to continue syncing.") : page.api.lastError;
                                    regResult.problem = !ok;
                                    if (ok) {
                                        regClient.text = Qt.binding(() => page.api.clientId);
                                        regTenant.text = Qt.binding(() => page.api.tenantId);
                                    }
                                }
                            }
                        }
                        QQC2.Label {
                            id: regResult
                            property bool problem: false
                            Layout.fillWidth: true
                            visible: text.length > 0
                            wrapMode: Text.Wrap
                            color: problem ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                        }

                        Kirigami.Heading {
                            Layout.fillWidth: true
                            Layout.topMargin: Kirigami.Units.largeSpacing
                            level: 3
                            text: i18n("Troubleshooting")
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: i18n("Compares every file on this computer with OneDrive and SharePoint again. Use this when something looks out of sync; nothing is deleted by it.")
                        }
                        RowLayout {
                            QQC2.Button {
                                icon.name: "view-refresh"
                                text: i18n("Check All Files Again")
                                enabled: page.api.daemonRunning
                                onClicked: {
                                    page.api.recheckAll();
                                    recheckDone.visible = true;
                                }
                            }
                            QQC2.Label {
                                id: recheckDone
                                visible: false
                                text: i18n("Checking; this can take a while for large libraries.")
                            }
                        }

                        Item {
                            Layout.fillHeight: true
                        }
                    }

                    // About
                    ColumnLayout {
                        spacing: Kirigami.Units.largeSpacing
                        RowLayout {
                            spacing: Kirigami.Units.largeSpacing
                            Kirigami.Icon {
                                source: "io.github.nvaert1986.graphsync"
                                implicitWidth: Kirigami.Units.iconSizes.huge
                                implicitHeight: Kirigami.Units.iconSizes.huge
                            }
                            ColumnLayout {
                                Kirigami.Heading {
                                    level: 2
                                    text: i18n("Graph Sync")
                                }
                                QQC2.Label {
                                    text: i18n("Version %1", page.api.version)
                                }
                            }
                        }
                        Kirigami.FormLayout {
                            QQC2.Label {
                                Kirigami.FormData.label: i18n("License:")
                                text: i18n("GNU General Public License, version 3 or later")
                            }
                            QQC2.Label {
                                Kirigami.FormData.label: i18n("Sync state:")
                                text: page.api.stateFolder
                            }
                            QQC2.Label {
                                Kirigami.FormData.label: i18n("Logs:")
                                font.family: "monospace"
                                text: "journalctl --user -u plasma-graphsync-linux"
                            }
                        }
                        QQC2.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            opacity: 0.7
                            text: i18n("Graph Sync is not affiliated with or endorsed by Microsoft. OneDrive and SharePoint are trademarks of Microsoft Corporation.")
                        }
                        Item {
                            Layout.fillHeight: true
                        }
                    }
                }
            }
        }
    }
}
