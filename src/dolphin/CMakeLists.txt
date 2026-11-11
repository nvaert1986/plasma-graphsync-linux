# SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Status emblems; loaded by Dolphin and the KIO file dialogs
kcoreaddons_add_plugin(graphsyncoverlayplugin SOURCES overlayplugin.cpp INSTALL_NAMESPACE "kf6/overlayicon")
target_link_libraries(graphsyncoverlayplugin PRIVATE Qt6::Network KF6::KIOCore)

# Context menu: Share, Copy Link, View Online, Version History, Sync Now, Choose Folders, Settings
kcoreaddons_add_plugin(graphsyncactionplugin SOURCES actionplugin.cpp INSTALL_NAMESPACE "kf6/kfileitemaction")
target_link_libraries(graphsyncactionplugin PRIVATE Qt6::DBus Qt6::Widgets KF6::ConfigCore KF6::I18n KF6::KIOWidgets)
target_compile_definitions(graphsyncactionplugin PRIVATE TRANSLATION_DOMAIN="plasma-graphsync-linux")
