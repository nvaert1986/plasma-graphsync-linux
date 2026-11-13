// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

// graphsyncd on the session bus. The Dolphin and thumbnail plugins (which do not link the core) and
// the Q_CLASSINFO of the service itself spell these out; keep them the same.
namespace GraphSync::DBusNames
{
inline QString service()
{
    return QStringLiteral("io.github.nvaert1986.GraphSync");
}
inline QString path()
{
    return QStringLiteral("/GraphSync");
}
inline QString interfaceName()
{
    return QStringLiteral("io.github.nvaert1986.GraphSync");
}
}
