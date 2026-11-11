// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ignorerules.h"

#include <QStringList>

namespace GraphSync::IgnoreRules
{

bool isIgnored(const QString &name)
{
    if (name.startsWith(tempPrefix) || name.startsWith(legacyTempPrefix)) {
        return true;
    }
    // LibreOffice locks (.~lock.name#), Office owner files (~$name), editor backups
    if (name.startsWith(QLatin1String(".~lock.")) || name.startsWith(QLatin1String("~$")) || name.endsWith(QLatin1Char('~'))) {
        return true;
    }
    // ONLYOFFICE and other editors keep scratch copies next to the document
    if (name.startsWith(QLatin1String(".~")) || name.endsWith(QLatin1String(".tmp")) || name.endsWith(QLatin1String(".part"))
        || name.endsWith(QLatin1String(".crdownload")) || name.endsWith(QLatin1String(".swp"))) {
        return true;
    }
    if (name.startsWith(QLatin1String(".Trash-"))) {
        return true;
    }
    static const QStringList exact = {QStringLiteral(".directory"), QStringLiteral("desktop.ini"), QStringLiteral("Thumbs.db"),
                                      QStringLiteral(".DS_Store")};
    return exact.contains(name);
}

QString invalidRemoteNameReason(const QString &name)
{
    static const QString forbiddenChars = QStringLiteral("\"*:<>?/\\|");
    for (const QChar c : name) {
        if (forbiddenChars.contains(c)) {
            return QStringLiteral("contains the character %1, which SharePoint does not allow").arg(c);
        }
    }
    if (name.startsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char(' '))) {
        return QStringLiteral("starts or ends with a space");
    }
    if (name.endsWith(QLatin1Char('.'))) {
        return QStringLiteral("ends with a dot");
    }
    static const QStringList reserved = {QStringLiteral(".lock"), QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
                                         QStringLiteral("NUL"), QStringLiteral("_vti_"), QStringLiteral("desktop.ini")};
    const QString base = name.section(QLatin1Char('.'), 0, 0).toUpper();
    if (reserved.contains(name, Qt::CaseInsensitive) || reserved.contains(base)
        || (base.size() == 4 && (base.startsWith(QLatin1String("COM")) || base.startsWith(QLatin1String("LPT"))) && base.at(3).isDigit())
        || name.contains(QLatin1String("_vti_"))) {
        return QStringLiteral("is a name SharePoint reserves");
    }
    if (name.size() > 255) {
        return QStringLiteral("is longer than 255 characters");
    }
    return {};
}

UserRules::UserRules(const QStringList &patterns, bool skipDotFiles)
    : m_skipDotFiles(skipDotFiles)
{
    for (const QString &pattern : patterns) {
        const QString trimmed = pattern.trimmed();
        if (!trimmed.isEmpty()) {
            m_patterns.append(QRegularExpression::fromWildcard(trimmed, Qt::CaseInsensitive));
        }
    }
}

bool UserRules::matchesName(const QString &name) const
{
    if (name == noSyncMarker || (m_skipDotFiles && name.startsWith(QLatin1Char('.')))) {
        return true;
    }
    for (const QRegularExpression &pattern : m_patterns) {
        if (pattern.match(name).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool UserRules::matchesPath(const QString &rel) const
{
    const QStringList parts = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (matchesName(part)) {
            return true;
        }
    }
    return false;
}

}
