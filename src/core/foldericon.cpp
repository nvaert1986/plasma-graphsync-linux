// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "foldericon.h"

#include <KConfig>
#include <KConfigGroup>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

namespace GraphSync::FolderIcon
{

void apply(const QString &folder, const QString &iconName)
{
    if (!QFileInfo(folder).isDir()) {
        return;
    }
    KConfig directory(QDir(folder).filePath(QStringLiteral(".directory")), KConfig::SimpleConfig);
    KConfigGroup group = directory.group(QStringLiteral("Desktop Entry"));
    if (group.hasKey("Icon")) {
        return;
    }
    group.writeEntry("Icon", iconName);
    directory.sync();

    // Tell open Dolphin windows (KDirLister) to re-read the folder, so the icon shows without F5
    QDBusMessage message = QDBusMessage::createSignal(QStringLiteral("/"), QStringLiteral("org.kde.KDirNotify"), QStringLiteral("FilesChanged"));
    message << QStringList{QUrl::fromLocalFile(folder).toString()};
    QDBusConnection::sessionBus().send(message);
}

}
