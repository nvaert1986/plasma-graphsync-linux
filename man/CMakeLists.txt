# SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

foreach(page graphsyncctl graphsyncd plasma-graphsync-linux)
    configure_file(${page}.1.in ${CMAKE_CURRENT_BINARY_DIR}/${page}.1 @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${page}.1 DESTINATION ${KDE_INSTALL_MANDIR}/man1)
endforeach()
