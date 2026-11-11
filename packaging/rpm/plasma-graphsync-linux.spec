# SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Build: rpmbuild -ba --define "_sourcedir $PWD" plasma-graphsync-linux.spec
# (with plasma-graphsync-linux-<version>.tar.gz next to it). Leave the app registration out:
# users enter it in the setup wizard. To ship one in /etc/xdg/graphsyncrc, add
# --define "graphsync_client_id <id>" --define "graphsync_tenant_id <id>".

Name:           plasma-graphsync-linux
Version:        0.7.2
Release:        1%{?dist}
Summary:        Sync OneDrive for Business and SharePoint libraries on KDE Plasma
License:        GPL-3.0-or-later AND LGPL-3.0-or-later
URL:            https://github.com/nvaert1986/plasma-graphsync-linux
Source0:        %{url}/archive/refs/tags/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  extra-cmake-modules
BuildRequires:  gettext
BuildRequires:  systemd-rpm-macros
BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6NetworkAuth)
BuildRequires:  cmake(Qt6Qml)
BuildRequires:  cmake(Qt6Quick)
BuildRequires:  cmake(Qt6QuickControls2)
BuildRequires:  cmake(Qt6Sql)
BuildRequires:  cmake(Qt6Test)
BuildRequires:  cmake(Qt6WebSockets)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6Keychain)
BuildRequires:  cmake(KF6Bookmarks)
BuildRequires:  cmake(KF6Config)
BuildRequires:  cmake(KF6CoreAddons)
BuildRequires:  cmake(KF6DBusAddons)
BuildRequires:  cmake(KF6GuiAddons)
BuildRequires:  cmake(KF6I18n)
BuildRequires:  cmake(KF6KCMUtils)
BuildRequires:  cmake(KF6KIO)
BuildRequires:  cmake(KF6Kirigami)
BuildRequires:  cmake(KF6Notifications)
BuildRequires:  cmake(KF6WindowSystem)
BuildRequires:  cmake(Plasma)

# Loaded at run time, so rpm cannot find them by itself: the QML modules of the windows, the
# tray applet (org.kde.plasma.workspace.dbus comes with plasma-workspace) and the SQLite driver
Requires:       kf6-kirigami
Requires:       kf6-kcmutils
Requires:       plasma-workspace
Requires:       qt6-qtdeclarative
Requires:       qt6-qtbase
Requires:       xdg-utils
# Called plasma-onedrive-linux before 0.7.0; this package takes its place
Obsoletes:      plasma-onedrive-linux < 0.7.0
Provides:       plasma-onedrive-linux = %{version}-%{release}

%description
Keeps OneDrive for Business and SharePoint document libraries in folders on
this computer, with a system tray applet, a System Settings page, Dolphin
integration (sync emblems, Share, Copy Link, Version History) and support for
the Sync button in SharePoint. Not affiliated with Microsoft.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DBUILD_TESTING=OFF \
       -DGRAPHSYNC_CLIENT_ID="%{?graphsync_client_id}" \
       -DGRAPHSYNC_TENANT_ID="%{?graphsync_tenant_id}"
%cmake_build

%install
%cmake_install
%find_lang %{name} --all-name

%files -f %{name}.lang
%license LICENSES/GPL-3.0-or-later.txt LICENSES/LGPL-3.0-or-later.txt
%doc README.md INSTALL.md CHANGELOG.md
%{_bindir}/plasma-graphsync-linux
%{_bindir}/graphsyncctl
%{_bindir}/graphsyncd
%{_qt6_plugindir}/kf6/kfileitemaction/graphsyncactionplugin.so
%{_qt6_plugindir}/kf6/overlayicon/graphsyncoverlayplugin.so
%{_qt6_plugindir}/plasma/kcms/systemsettings/kcm_graphsync.so
%{_datadir}/applications/io.github.nvaert1986.graphsync.desktop
%{_datadir}/applications/io.github.nvaert1986.graphsync.download.desktop
%{_datadir}/applications/kcm_graphsync.desktop
%{_datadir}/dbus-1/services/io.github.nvaert1986.GraphSync.service
%{_datadir}/icons/hicolor/scalable/apps/io.github.nvaert1986.graphsync.svg
%{_datadir}/icons/hicolor/scalable/emblems/io.github.nvaert1986.graphsync-emblem-*.svg
%{_datadir}/icons/hicolor/scalable/status/io.github.nvaert1986.graphsync-*.svg
%{_datadir}/knotifications6/plasma-graphsync-linux.notifyrc
%{_metainfodir}/io.github.nvaert1986.graphsync.metainfo.xml
%{_datadir}/mime/packages/io.github.nvaert1986.graphsync.placeholder.xml
%{_datadir}/plasma/plasmoids/io.github.nvaert1986.graphsync.tray/
%{_sysconfdir}/xdg/autostart/io.github.nvaert1986.graphsync.migrate.desktop
%{_userunitdir}/plasma-graphsync-linux.service
%if 0%{?graphsync_client_id:1}
%config(noreplace) %{_sysconfdir}/xdg/graphsyncrc
%endif

%changelog
* Wed Nov 11 2026 nvaert1986 <25772781+nvaert1986@users.noreply.github.com> - 0.7.2-1
- Security and reliability fixes; see CHANGELOG.md.

* Wed Nov 11 2026 nvaert1986 <25772781+nvaert1986@users.noreply.github.com> - 0.7.1-1
- Safer takeover of plasma-onedrive-linux installs; fixes from a code review.

* Wed Nov 11 2026 nvaert1986 <25772781+nvaert1986@users.noreply.github.com> - 0.7.0-1
- New name (was plasma-onedrive-linux); an existing install is taken over at login.

* Tue Nov 10 2026 nvaert1986 <25772781+nvaert1986@users.noreply.github.com> - 0.6.5-1
- Setup wizard asks for the app registration; first packages for Fedora.
