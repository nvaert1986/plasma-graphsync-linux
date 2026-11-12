# SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(graphsynccore STATIC)
# Linked into the System Settings and Dolphin plugins (shared objects) too
set_target_properties(graphsynccore PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(graphsynccore PRIVATE
    core/accounts.cpp
    core/auth.cpp
    core/config.cpp
    core/driveitem.cpp
    core/foldericon.cpp
    core/graphclient.cpp
    core/ignorerules.cpp
    core/itemservice.cpp
    core/localscanner.cpp
    core/localwatcher.cpp
    core/migration.cpp
    core/pushclient.cpp
    core/placeholder.cpp
    core/quickxorhash.cpp
    core/ratelimiter.cpp
    core/statedb.cpp
    core/syncengine.cpp
)

ecm_qt_declare_logging_category(graphsynccore
    HEADER graphsync_debug.h
    IDENTIFIER GRAPHSYNC
    CATEGORY_NAME graphsync
    DEFAULT_SEVERITY Info
    DESCRIPTION "plasma-graphsync-linux"
    EXPORT GRAPHSYNC
)

target_compile_definitions(graphsynccore PRIVATE TRANSLATION_DOMAIN="plasma-graphsync-linux")
target_include_directories(graphsynccore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(graphsynccore PUBLIC
    Qt6::Core
    Qt6::DBus
    Qt6::Network
    Qt6::NetworkAuth
    Qt6::Sql
    Qt6::WebSockets
    KF6::ConfigCore
    KF6::I18n
    Qt6Keychain::Qt6Keychain
)

add_executable(graphsyncctl graphsyncctl/main.cpp)
target_link_libraries(graphsyncctl PRIVATE graphsynccore Qt6::DBus)
target_compile_definitions(graphsyncctl PRIVATE TRANSLATION_DOMAIN="plasma-graphsync-linux")

add_executable(graphsyncd graphsyncd/main.cpp graphsyncd/daemon.cpp graphsyncd/rootcontroller.cpp graphsyncd/dbusservice.cpp graphsyncd/socketserver.cpp graphsyncd/places.cpp)
target_link_libraries(graphsyncd PRIVATE graphsynccore Qt6::DBus Qt6::Gui KF6::Bookmarks KF6::GuiAddons KF6::Notifications)
target_compile_definitions(graphsyncd PRIVATE TRANSLATION_DOMAIN="plasma-graphsync-linux")

add_executable(plasma-graphsync-linux app/main.cpp app/backend.cpp)
target_compile_definitions(plasma-graphsync-linux PRIVATE TRANSLATION_DOMAIN="plasma-graphsync-linux")
qt_add_qml_module(plasma-graphsync-linux
    URI io.github.nvaert1986.graphsync.app
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES
        app/qml/Main.qml
        app/qml/SettingsPage.qml
        app/qml/SettingsView.qml
        app/qml/SetupWizard.qml
        app/qml/LibraryPicker.qml
        app/qml/AddLibraryPage.qml
        app/qml/OdopenPage.qml
        app/qml/ChooseFoldersPage.qml
        app/qml/SignInPage.qml
        app/qml/SharePage.qml
        app/qml/VersionsPage.qml
        app/qml/DeviceCodeBox.qml
        app/qml/ImportPage.qml
        app/qml/AddAccountPage.qml
)
target_link_libraries(plasma-graphsync-linux PRIVATE
    graphsynccore
    Qt6::DBus
    Qt6::Widgets
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    KF6::CoreAddons
    KF6::I18n
    KF6::I18nQml
    KF6::DBusAddons
    KF6::WindowSystem
)

install(TARGETS graphsyncctl graphsyncd plasma-graphsync-linux ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

add_subdirectory(kcm)
add_subdirectory(dolphin)
