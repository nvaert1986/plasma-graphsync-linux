# SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Previews of online-only files in Dolphin and the file dialogs, fetched through graphsyncd
kcoreaddons_add_plugin(graphsyncthumbnail SOURCES thumbnailer.cpp INSTALL_NAMESPACE "kf6/thumbcreator")
target_link_libraries(graphsyncthumbnail PRIVATE Qt6::DBus Qt6::Gui KF6::KIOGui)
