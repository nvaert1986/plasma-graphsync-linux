# SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# The page shares the app's Backend and settings view, so there is one implementation of both
kcmutils_add_qml_kcm(kcm_graphsync SOURCES kcm_graphsync.cpp ../app/backend.cpp)
target_include_directories(kcm_graphsync PRIVATE ../app)
# SettingsView.qml next to ui/main.qml in the module's resources, so main.qml finds it by name
qt_add_resources(kcm_graphsync "kcm_graphsync_shared"
    PREFIX "/kcm/kcm_graphsync"
    BASE ../app/qml
    FILES ../app/qml/SettingsView.qml)
target_link_libraries(kcm_graphsync PRIVATE graphsynccore Qt6::DBus Qt6::Gui KF6::KCMUtilsQuick KF6::CoreAddons KF6::I18n KF6::KIOGui)
# KCMs use their plugin id as translation catalog
target_compile_definitions(kcm_graphsync PRIVATE TRANSLATION_DOMAIN="kcm_graphsync")
