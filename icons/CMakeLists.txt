# SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

install(FILES io.github.nvaert1986.graphsync.svg DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps)
install(FILES
    io.github.nvaert1986.graphsync-idle.svg
    io.github.nvaert1986.graphsync-syncing.svg
    io.github.nvaert1986.graphsync-paused.svg
    io.github.nvaert1986.graphsync-error.svg
    io.github.nvaert1986.graphsync-offline.svg
    DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/status)
install(FILES
    io.github.nvaert1986.graphsync-emblem-syncing.svg
    io.github.nvaert1986.graphsync-emblem-cloud.svg
    DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/emblems)
