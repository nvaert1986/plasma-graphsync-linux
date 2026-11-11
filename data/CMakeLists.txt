# SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

configure_file(plasma-graphsync-linux.service.in ${CMAKE_CURRENT_BINARY_DIR}/plasma-graphsync-linux.service @ONLY)
configure_file(io.github.nvaert1986.GraphSync.service.in ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.GraphSync.service @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/plasma-graphsync-linux.service DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.GraphSync.service DESTINATION ${KDE_INSTALL_DBUSSERVICEDIR})
install(FILES plasma-graphsync-linux.notifyrc DESTINATION ${KDE_INSTALL_KNOTIFYRCDIR})

configure_file(io.github.nvaert1986.graphsync.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.graphsync.desktop @ONLY)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.graphsync.desktop DESTINATION ${KDE_INSTALL_APPDIR})
# Open With > Download Now for placeholders, also in the KDE file dialog (hidden from menus)
configure_file(io.github.nvaert1986.graphsync.download.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.graphsync.download.desktop @ONLY)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.graphsync.download.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES io.github.nvaert1986.graphsync.metainfo.xml DESTINATION ${KDE_INSTALL_METAINFODIR})

# Takes over an install of plasma-onedrive-linux (the name before 0.7.0) once per user at login
configure_file(io.github.nvaert1986.graphsync.migrate.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.graphsync.migrate.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.graphsync.migrate.desktop DESTINATION ${KDE_INSTALL_AUTOSTARTDIR})

# Files On-Demand placeholders (.cloud); packages refresh the MIME database in their own hooks
install(FILES io.github.nvaert1986.graphsync.placeholder.xml DESTINATION ${KDE_INSTALL_MIMEDIR})
find_program(UPDATE_MIME_DATABASE update-mime-database)
if(UPDATE_MIME_DATABASE)
    install(CODE "if(\"\$ENV{DESTDIR}\" STREQUAL \"\")
        execute_process(COMMAND ${UPDATE_MIME_DATABASE} \"${KDE_INSTALL_FULL_DATADIR}/mime\")
    endif()")
endif()

if(GRAPHSYNC_CLIENT_ID AND GRAPHSYNC_TENANT_ID)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/graphsyncrc "[Auth]\nClientId=${GRAPHSYNC_CLIENT_ID}\nTenantId=${GRAPHSYNC_TENANT_ID}\n")
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/graphsyncrc DESTINATION ${KDE_INSTALL_CONFDIR})
endif()
