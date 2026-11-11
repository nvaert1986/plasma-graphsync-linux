// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Sites the user follows, a search box that also accepts a pasted SharePoint address, and
// per site the document libraries to tick. `selected` holds the ticked libraries.
ColumnLayout {
    id: picker

    property var selected: []
    property var sites: []
    property bool searching: false
    property var librariesBySite: ({})
    property var expanded: ({})

    function isSelected(driveId) {
        return selected.some(entry => entry.driveId === driveId);
    }

    function toggle(site, library, on) {
        const rest = selected.filter(entry => entry.driveId !== library.id);
        if (on) {
            rest.push({
                siteName: site.name,
                libraryName: library.name,
                driveId: library.id,
                webUrl: library.webUrl
            });
        }
        selected = rest;
    }

    function loadFollowed() {
        searching = false;
        sites = backend.followedSites();
    }

    function search(text) {
        const query = text.trim();
        if (query.length === 0) {
            loadFollowed();
            return;
        }
        searching = true;
        if (query.startsWith("https://")) {
            const site = backend.siteFromUrl(query);
            sites = site.id ? [site] : [];
            if (site.id) {
                setExpanded(site, true);
            }
        } else {
            sites = backend.searchSites(query);
        }
    }

    function setExpanded(site, on) {
        if (on && librariesBySite[site.id] === undefined) {
            const copy = Object.assign({}, librariesBySite);
            copy[site.id] = backend.libraries(site.id);
            librariesBySite = copy;
        }
        const state = Object.assign({}, expanded);
        state[site.id] = on;
        expanded = state;
    }

    spacing: Kirigami.Units.smallSpacing

    // Only once signed in: the setup wizard creates this page before the sign-in, and a Graph
    // call then would greet a new user with an error
    Component.onCompleted: {
        if (backend.signedIn) {
            loadFollowed();
        }
    }

    Connections {
        target: backend
        function onAccountChanged() {
            if (backend.signedIn && !picker.searching && picker.sites.length === 0) {
                picker.loadFollowed();
            }
        }
    }

    Kirigami.SearchField {
        id: searchField
        Layout.fillWidth: true
        placeholderText: i18n("Search for a site, or paste its address")
        autoAccept: false
        onAccepted: picker.search(text)
    }

    QQC2.Label {
        Layout.fillWidth: true
        text: picker.searching ? i18n("Search results") : i18n("Sites you follow")
        font.bold: true
    }

    QQC2.ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: Kirigami.Units.gridUnit * 8

        ListView {
            id: siteList
            model: picker.sites
            clip: true
            spacing: 0

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                visible: siteList.count === 0 && !backend.busy
                icon.name: "folder-cloud"
                text: picker.searching ? i18n("No sites found") : i18n("You do not follow any sites yet")
                explanation: i18n("Search for a site by name, or paste the address of a SharePoint site or library. New sites can take a while to appear in search; pasting the address always works.")
            }

            delegate: ColumnLayout {
                id: siteDelegate
                required property var modelData
                readonly property bool open: picker.expanded[modelData.id] === true
                width: ListView.view.width
                spacing: 0

                QQC2.ItemDelegate {
                    Layout.fillWidth: true
                    icon.name: siteDelegate.open ? "go-down-symbolic" : "go-next-symbolic"
                    text: siteDelegate.modelData.name
                    onClicked: picker.setExpanded(siteDelegate.modelData, !siteDelegate.open)
                }

                Repeater {
                    model: siteDelegate.open ? (picker.librariesBySite[siteDelegate.modelData.id] ?? []) : []
                    delegate: QQC2.CheckDelegate {
                        required property var modelData
                        Layout.fillWidth: true
                        leftPadding: Kirigami.Units.gridUnit * 2
                        icon.name: "folder-documents"
                        text: modelData.name
                        checked: picker.isSelected(modelData.id)
                        onToggled: picker.toggle(siteDelegate.modelData, modelData, checked)
                    }
                }
            }
        }
    }
}
