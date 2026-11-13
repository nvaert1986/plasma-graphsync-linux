// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "placeholder.h"
#include "localscanner.h"

#include <KLocalizedString>

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>


#include <algorithm>

namespace GraphSync::Placeholder
{

namespace
{
// A real placeholder is a few hundred bytes; anything bigger is not one
constexpr qint64 maxPlaceholderSize = 4096;
}

QString pathFor(const QString &realPath)
{
    return realPath + suffix;
}

QString realPathOf(const QString &placeholderPath)
{
    return hasSuffix(placeholderPath) ? placeholderPath.chopped(suffix.size()) : placeholderPath;
}

bool hasSuffix(const QString &path)
{
    return path.endsWith(suffix) && path.size() > suffix.size();
}

bool write(const QString &realPath, qint64 size, const QDateTime &modified, QString *error)
{
    const QString name = QFileInfo(realPath).fileName();
    QSaveFile file(pathFor(realPath));
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }
    const QString text = QString(firstLine) + QLatin1Char('\n')
        + i18n("%1 is only online (%2). Open this file to download it, or right-click it in Dolphin and choose \"Always Keep on This Device\".",
               name,
               QLocale().formattedDataSize(size))
        + QLatin1Char('\n') + i18n("Do not edit or rename this file; it is replaced by the real file when that is downloaded.") + QLatin1Char('\n');
    file.write(text.toUtf8());
    if (!file.commit()) {
        *error = file.errorString();
        return false;
    }
    // The real file's date, so sorting by date in Dolphin keeps making sense
    LocalScanner::setModified(pathFor(realPath), modified);
    return true;
}

bool isPlaceholder(const QString &path)
{
    QFile file(path);
    if (file.size() > maxPlaceholderSize || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray line = file.readLine(std::max(firstLine.size(), legacyFirstLine.size()) + 2).trimmed();
    return line == QByteArrayView(firstLine.data(), firstLine.size()) || line == QByteArrayView(legacyFirstLine.data(), legacyFirstLine.size());
}

}
