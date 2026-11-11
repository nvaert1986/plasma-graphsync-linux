// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: window

    // Pages opened on top of the settings return to them; opened on their own, they quit
    function closePage() {
        if (pageStack.depth > 1) {
            pageStack.pop();
        } else {
            Qt.quit();
        }
    }

    function open(mode, argument) {
        // A second start while setup runs must not interrupt the wizard, nor the import
        if ((pageStack.currentItem instanceof SetupWizard && mode !== "odopen") || pageStack.currentItem instanceof ImportPage) {
            return;
        }
        const base = pageStack.depth > 0 && pageStack.get(0) instanceof SettingsPage;
        switch (mode) {
        case "settings":
            if (base) {
                pageStack.pop(pageStack.get(0));
            } else {
                pageStack.clear();
                pageStack.push(settingsComponent);
            }
            break;
        case "odopen":
            pageStack.push(odopenComponent, { link: argument });
            break;
        case "chooseFolders":
            pageStack.push(chooseComponent, { rootId: argument });
            break;
        case "share":
        case "versions": {
            const component = mode === "share" ? shareComponent : versionsComponent;
            // A dialog opened on its own is replaced by the next one; on top of the settings it stacks
            const standalone = pageStack.depth === 1 && !base;
            if (standalone) {
                pageStack.replace(component, { path: argument });
            } else {
                pageStack.push(component, { path: argument });
            }
            break;
        }
        case "signIn":
            pageStack.push(signInComponent);
            break;
        case "add":
            pageStack.push(addComponent);
            break;
        case "setup":
            pageStack.clear();
            pageStack.push(setupComponent);
            break;
        }
    }

    title: i18n("Graph Sync")
    // Room for two libraries with their buttons on one row
    width: Kirigami.Units.gridUnit * 48
    height: Kirigami.Units.gridUnit * 40
    minimumWidth: Kirigami.Units.gridUnit * 28
    minimumHeight: Kirigami.Units.gridUnit * 22

    pageStack.globalToolBar.style: Kirigami.ApplicationHeaderStyle.ToolBar
    pageStack.globalToolBar.showNavigationButtons: Kirigami.ApplicationHeaderStyle.ShowBackButton
    pageStack.columnView.columnResizeMode: Kirigami.ColumnView.SingleColumn

    Component {
        id: settingsComponent
        SettingsPage {}
    }
    Component {
        id: importComponent
        ImportPage {}
    }
    Component {
        id: setupComponent
        SetupWizard {}
    }
    Component {
        id: addComponent
        AddLibraryPage {}
    }
    Component {
        id: odopenComponent
        OdopenPage {}
    }
    Component {
        id: chooseComponent
        ChooseFoldersPage {}
    }
    Component {
        id: signInComponent
        SignInPage {}
    }
    Component {
        id: shareComponent
        SharePage {}
    }
    Component {
        id: versionsComponent
        VersionsPage {}
    }

    Connections {
        target: backend
        function onActivateRequested(mode, argument) {
            window.open(mode, argument);
        }
    }

    function start() {
        // The start page was chosen before an import of plasma-onedrive-linux finished: with the
        // setup taken over, the wizard is no longer what to show
        const mode = startMode === "setup" && backend.setupDone ? "settings" : startMode;
        // Everything except the wizard sits on top of the settings, like OneDrive's windows;
        // Share and Version history from Dolphin or the tray open as a dialog of their own
        const standalone = mode === "setup" || mode === "share" || mode === "versions";
        if (!standalone) {
            pageStack.push(settingsComponent);
        }
        if (mode !== "settings") {
            open(mode, startArgument);
        }
    }

    Connections {
        target: backend
        function onImportFinished() {
            if (backend.importState === "" && pageStack.currentItem instanceof ImportPage) {
                pageStack.clear();
                window.start();
            }
        }
    }

    Component.onCompleted: {
        if (backend.importState !== "") {
            pageStack.push(importComponent);
        } else {
            start();
        }
    }
}
