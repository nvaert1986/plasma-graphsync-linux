// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QString>

// Files On-Demand placeholders: an online-only file report.docx is on disk as
// report.docx.cloud, a small text file that says what it stands for. Its first line is fixed, so
// a placeholder is recognised by content too: one renamed or copied by hand is never uploaded.
namespace GraphSync::Placeholder
{

inline constexpr QLatin1StringView suffix{".cloud"};
inline constexpr QLatin1StringView firstLine{"[plasma-graphsync-linux placeholder]"};
// Written by plasma-onedrive-linux, the name of this client before 0.7.0; still recognised
inline constexpr QLatin1StringView legacyFirstLine{"[plasma-onedrive-linux placeholder]"};

QString pathFor(const QString &realPath);
// The real file a placeholder stands for (the path without the suffix)
QString realPathOf(const QString &placeholderPath);
bool hasSuffix(const QString &path);

// Writes (or rewrites) the placeholder for realPath. Returns false with an error when it fails.
bool write(const QString &realPath, qint64 size, const QDateTime &modified, QString *error);

// True when the file is one of our placeholders, whatever its name
bool isPlaceholder(const QString &path);

}
