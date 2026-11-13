# SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)

project(plasma-graphsync-linux VERSION 0.9.4 LANGUAGES CXX)

# Qt 6.9 and KDE Frameworks 6.20 are the main target. Older systems such as Debian 13 (Qt 6.8,
# Frameworks 6.13, Plasma 6.3) build too, but sign in with a code only (Auth::browserLoginAvailable)
set(QT_MIN_VERSION "6.8.0")
set(KF_MIN_VERSION "6.13.0")
set(KDE_COMPILERSETTINGS_LEVEL "6.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMQtDeclareLoggingCategory)
include(ECMAddTests)
include(FeatureSummary)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Gui Widgets Network NetworkAuth Sql DBus WebSockets Qml Quick QuickControls2 Test)
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS Bookmarks Config CoreAddons DBusAddons GuiAddons I18n KIO Notifications KCMUtils WindowSystem)
find_package(KF6Kirigami ${KF_MIN_VERSION})
find_package(Plasma REQUIRED)
set_package_properties(KF6Kirigami PROPERTIES TYPE RUNTIME DESCRIPTION "QML components of the setup app and settings page")
find_package(Qt6Keychain REQUIRED)

# The daemon, tray menu and settings page start the app by absolute path: a systemd user
# service does not necessarily have the install prefix (for example ~/.local/bin) in PATH
add_compile_definitions(GRAPHSYNC_VERSION="${PROJECT_VERSION}" GRAPHSYNC_BINDIR="${KDE_INSTALL_FULL_BINDIR}")

# Optional: build the app registration in, written to /etc/xdg/graphsyncrc (users can still override it)
set(GRAPHSYNC_CLIENT_ID "" CACHE STRING "Entra ID application (client) ID to ship in /etc/xdg/graphsyncrc")
set(GRAPHSYNC_TENANT_ID "" CACHE STRING "Entra ID directory (tenant) ID to ship in /etc/xdg/graphsyncrc")

add_subdirectory(src)
add_subdirectory(data)
add_subdirectory(icons)
add_subdirectory(man)

plasma_install_package(plasmoid/package io.github.nvaert1986.graphsync.tray)

ki18n_install(po)

if(BUILD_TESTING)
    add_subdirectory(autotests)
endif()

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
