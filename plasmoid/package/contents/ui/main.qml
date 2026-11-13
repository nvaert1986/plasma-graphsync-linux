// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.plasmoid
import org.kde.plasma.workspace.dbus as DBus

PlasmoidItem {
    id: root

    readonly property string service: "io.github.nvaert1986.GraphSync"
    readonly property bool daemonRunning: watcher.registered
    property var status: ({})
    property var activity: []

    readonly property var roots: status.roots ?? []
    // The current account's OneDrive (the header's folder button and storage)
    readonly property var personalRoot: roots.find(r => r.kind === "personal" && r.accountId === status.account?.id) ?? roots.find(r => r.kind === "personal") ?? null
    readonly property var accounts: status.accounts ?? []
    // Accounts with locations here whose sign-in is gone: each gets its own "Sign in again"
    readonly property var signedOutAccounts: accounts.filter(a => !a.signedIn && a.locations > 0)
    // Accounts this computer syncs with; with more than one, the popup shows each one's state
    readonly property var syncAccounts: accounts.filter(a => a.signedIn || a.locations > 0)
    readonly property bool syncing: roots.some(r => r.state === "syncing")
    readonly property var problemRoots: roots.filter(r => r.state === "error" || r.state === "halted" || (r.persistentErrors ?? []).length > 0)
    readonly property var bigDeletes: roots.filter(r => r.pendingBigDelete !== undefined)
    readonly property string syncState: {
        if (!daemonRunning) {
            return "offline";
        }
        // Signed out everywhere; one signed-out account among others is a problem of that account
        if (status.signedIn === false && !accounts.some(a => a.signedIn)) {
            return "signedOut";
        }
        if (status.paused) {
            return "paused";
        }
        // No network: grey, like Windows, not a problem to report
        if (roots.length > 0 && roots.every(r => r.state === "offline")) {
            return "offline";
        }
        if (bigDeletes.length > 0 || problemRoots.length > 0 || signedOutAccounts.length > 0) {
            return "error";
        }
        return syncing ? "syncing" : "idle";
    }
    readonly property string statusText: {
        switch (syncState) {
        case "offline":
            return daemonRunning ? i18n("Offline: syncing continues when the network is back") : i18n("The sync service is not running");
        case "signedOut":
            return i18n("Not signed in");
        case "paused":
            if (status.pausedForMeteredNetwork && !status.pausedByUser) {
                return i18n("Paused on a metered network");
            }
            return status.pausedUntil ? i18n("Paused until %1", Qt.formatTime(new Date(status.pausedUntil), Qt.locale().timeFormat(Locale.ShortFormat))) : i18n("Syncing is paused");
        case "error":
            if (bigDeletes.length > 0) {
                return i18n("Waiting for your decision");
            }
            if (problemRoots.length === 0) {
                return i18np("1 account is signed out", "%1 accounts are signed out", signedOutAccounts.length);
            }
            return i18np("1 library has a problem", "%1 libraries have problems", problemRoots.length);
        case "syncing":
            return i18n("Syncing…");
        default:
            return roots.length === 0 ? i18n("Nothing is synced yet") : i18n("Your files are synced");
        }
    }

    // The module hands back a pending-reply object: the method's return value is in
    // reply.value.value, an error in reply.error.message
    function unwrap(reply) {
        const value = reply?.value;
        return (value !== null && typeof value === "object") ? value.value : value;
    }

    function command(object, onResult) {
        // The one-argument asyncCall and its reply's finished signal: Plasma 6.3 (Debian 13) has
        // no asyncCall with result callbacks; it ignores them and the status never arrives
        const reply = DBus.SessionBus.asyncCall({
            service: service,
            path: "/GraphSync",
            iface: "io.github.nvaert1986.GraphSync",
            member: "Command",
            // Typed argument and no explicit signature: a plain JS string or "s" is rejected
            arguments: [new DBus.string(JSON.stringify(object))]
        });
        reply.finished.connect(() => {
            if (reply.isError) {
                console.warn("plasma-graphsync-linux:", reply.error?.message);
                return;
            }
            if (onResult) {
                const text = unwrap(reply);
                try {
                    onResult(JSON.parse(text));
                } catch (e) {
                    console.warn("plasma-graphsync-linux: unexpected reply", text);
                }
            }
        });
    }

    function refresh() {
        if (!daemonRunning) {
            status = {};
            activity = [];
            return;
        }
        command({ command: "status" }, result => root.status = result);
        command({ command: "activity" }, result => root.activity = result);
    }

    // A file:// URL for a local path; "#" and "%" in names must not cut or change the path
    function fileUrl(path) {
        return "file://" + path.split("/").map(encodeURIComponent).join("/");
    }

    // Signs in a signed-out account: the app signs in the current account, so make it that one
    function signIn(accountId) {
        command({ command: "setCurrentAccount", id: accountId }, () => launch(["--sign-in"]));
    }

    function launch(args) {
        command({ command: "launch", arguments: args });
    }

    Plasmoid.icon: {
        switch (syncState) {
        case "syncing":
            return "io.github.nvaert1986.graphsync-syncing";
        case "paused":
            return "io.github.nvaert1986.graphsync-paused";
        case "error":
            return "io.github.nvaert1986.graphsync-error";
        case "offline":
        case "signedOut":
            return "io.github.nvaert1986.graphsync-offline";
        default:
            return "io.github.nvaert1986.graphsync-idle";
        }
    }
    Plasmoid.status: syncState === "error" || syncState === "signedOut" ? PlasmaCore.Types.NeedsAttentionStatus : PlasmaCore.Types.ActiveStatus
    Plasmoid.title: i18n("Graph Sync")
    toolTipMainText: personalRoot ? personalRoot.name : i18n("Graph Sync")
    toolTipSubText: statusText

    // Right-click menu, like the OneDrive tray icon on Windows
    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            text: i18n("Open Folder")
            icon.name: "document-open-folder"
            visible: root.personalRoot !== null || root.roots.length > 0
            onTriggered: Qt.openUrlExternally(root.fileUrl((root.personalRoot ?? root.roots[0]).localPath))
        },
        PlasmaCore.Action {
            text: i18n("View Online")
            icon.name: "internet-services"
            visible: ((root.personalRoot ?? root.roots[0])?.webUrl ?? "").length > 0
            onTriggered: Qt.openUrlExternally((root.personalRoot ?? root.roots[0]).webUrl)
        },
        PlasmaCore.Action {
            isSeparator: true
        },
        PlasmaCore.Action {
            text: i18n("Resume Syncing")
            icon.name: "media-playback-start"
            visible: root.status.pausedByUser === true
            onTriggered: root.command({ command: "resume" })
        },
        PlasmaCore.Action {
            text: i18n("Pause for 2 Hours")
            icon.name: "media-playback-pause"
            visible: root.daemonRunning && root.status.pausedByUser !== true
            onTriggered: root.command({ command: "pause", minutes: 120 })
        },
        PlasmaCore.Action {
            text: i18n("Pause for 8 Hours")
            icon.name: "media-playback-pause"
            visible: root.daemonRunning && root.status.pausedByUser !== true
            onTriggered: root.command({ command: "pause", minutes: 480 })
        },
        PlasmaCore.Action {
            text: i18n("Pause for 24 Hours")
            icon.name: "media-playback-pause"
            visible: root.daemonRunning && root.status.pausedByUser !== true
            onTriggered: root.command({ command: "pause", minutes: 1440 })
        },
        PlasmaCore.Action {
            isSeparator: true
        },
        PlasmaCore.Action {
            text: i18n("Settings")
            icon.name: "configure"
            onTriggered: root.launch([])
        },
        PlasmaCore.Action {
            text: i18n("Quit Graph Sync")
            icon.name: "application-exit"
            visible: root.daemonRunning
            onTriggered: root.command({ command: "quit" })
        }
    ]

    fullRepresentation: FullRepresentation {
        app: root
    }

    DBus.DBusServiceWatcher {
        id: watcher
        busType: DBus.BusType.Session
        watchedService: root.service
        onRegisteredChanged: root.refresh()
    }

    Timer {
        // D-Bus signals are not available to QML here; status is cheap to poll locally
        interval: root.expanded ? 1500 : 5000
        running: root.daemonRunning
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    onExpandedChanged: () => {
        if (root.expanded) {
            refresh();
        }
    }
}
