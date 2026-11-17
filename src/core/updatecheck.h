// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The core's header of the same name: always with its folder
#include <graphcore/updatecheck.h>

#include <QObject>
#include <QString>

namespace GraphSync
{

// New versions: the packages on GitHub come from no repository, so no package manager tells their
// users about a new release. The check itself is the suite's (GraphCore::UpdateChecker, since
// 0.12.0); this says which repository and version, and where its answer is kept.
namespace UpdateCheck
{
// Built with the check (CMake option GRAPHSYNC_UPDATE_CHECK); distributions that ship updates
// themselves leave it out
bool compiledIn();
// The version running here
QString currentVersion();
// Graph Sync's repository, version, state file and User-Agent
GraphCore::UpdateCheckSettings settings();

using GraphCore::UpdateCheck::cleanVersion;
using GraphCore::UpdateCheck::isNewer;
}

// GraphCore's checker with Graph Sync's settings
class UpdateChecker : public GraphCore::UpdateChecker
{
public:
    explicit UpdateChecker(QObject *parent = nullptr)
        : GraphCore::UpdateChecker(UpdateCheck::settings(), parent)
    {
    }
};

}
