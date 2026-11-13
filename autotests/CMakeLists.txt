# SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

ecm_add_tests(
    quickxorhashtest.cpp
    ignorerulestest.cpp
    pushclienttest.cpp
    statedbtest.cpp
    authtest.cpp
    driveitemtest.cpp
    accountstest.cpp
    authlocktest.cpp
    autofreeuptest.cpp
    folderbackuptest.cpp
    dolphinpreviewstest.cpp
    graphclienttest.cpp
    LINK_LIBRARIES graphsynccore Qt6::Test
)
