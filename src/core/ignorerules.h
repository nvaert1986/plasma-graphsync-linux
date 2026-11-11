// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace GraphSync::IgnoreRules
{

// A folder holding a file with this name is not synced (nor is the marker itself)
inline constexpr QLatin1StringView noSyncMarker{".nosync"};

// What the user chose not to sync: name patterns and, optionally, all dot-files.
// Matching is case-insensitive, like SharePoint names.
class UserRules
{
public:
    UserRules() = default;
    UserRules(const QStringList &patterns, bool skipDotFiles);

    // One file or folder name
    bool matchesName(const QString &name) const;
    // Any component of a relative path
    bool matchesPath(const QString &rel) const;

private:
    QList<QRegularExpression> m_patterns;
    bool m_skipDotFiles = false;
};

// Prefix of our own partial downloads; never synced and cleaned up on start.
inline constexpr QLatin1StringView tempPrefix{".graphsync-tmp-"};
// The same from plasma-onedrive-linux, the name of this client before 0.7.0 (left by a daemon
// that was stopped mid-download before the switch)
inline constexpr QLatin1StringView legacyTempPrefix{".podl-tmp-"};

// Lock files, editor scratch files and desktop metadata that must never be uploaded.
bool isIgnored(const QString &name);

// SharePoint rejects some names; such local files are reported instead of uploaded.
// Returns an empty string when the name is fine, otherwise the reason.
QString invalidRemoteNameReason(const QString &name);

}
