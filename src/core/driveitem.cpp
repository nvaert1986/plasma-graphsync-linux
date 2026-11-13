// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "driveitem.h"

#include <QRegularExpression>

namespace GraphSync
{

DriveItem DriveItem::fromJson(const QJsonObject &json)
{
    DriveItem item;
    item.id = json.value(QLatin1String("id")).toString();
    item.parentId = json.value(QLatin1String("parentReference")).toObject().value(QLatin1String("id")).toString();
    item.name = json.value(QLatin1String("name")).toString();
    item.isFolder = json.contains(QLatin1String("folder"));
    item.isRoot = json.contains(QLatin1String("root"));
    item.deleted = json.contains(QLatin1String("deleted"));
    item.isRemoteShortcut = json.contains(QLatin1String("remoteItem"));
    if (item.isRemoteShortcut) {
        const QJsonObject remote = json.value(QLatin1String("remoteItem")).toObject();
        item.remoteId = remote.value(QLatin1String("id")).toString();
        item.remoteDriveId = remote.value(QLatin1String("parentReference")).toObject().value(QLatin1String("driveId")).toString();
        item.remoteWebUrl = remote.value(QLatin1String("webUrl")).toString();
        item.remoteIsFolder = remote.contains(QLatin1String("folder"));
    }
    item.shared = json.contains(QLatin1String("shared")) && !item.isRoot;
    item.size = json.value(QLatin1String("size")).toInteger();
    const QJsonObject modifiedBy = json.value(QLatin1String("lastModifiedBy")).toObject().value(QLatin1String("user")).toObject();
    item.modifiedById = modifiedBy.value(QLatin1String("id")).toString();
    item.modifiedByName = modifiedBy.value(QLatin1String("displayName")).toString();
    item.eTag = json.value(QLatin1String("eTag")).toString();
    item.cTag = json.value(QLatin1String("cTag")).toString();
    item.quickXorHash = json.value(QLatin1String("file"))
                            .toObject()
                            .value(QLatin1String("hashes"))
                            .toObject()
                            .value(QLatin1String("quickXorHash"))
                            .toString();
    // fileSystemInfo carries the client-side modification time; fall back to the server one
    QString modified = json.value(QLatin1String("fileSystemInfo")).toObject().value(QLatin1String("lastModifiedDateTime")).toString();
    if (modified.isEmpty()) {
        modified = json.value(QLatin1String("lastModifiedDateTime")).toString();
    }
    item.modified = QDateTime::fromString(modified, Qt::ISODateWithMs);
    return item;
}

QString DriveItem::selectFields()
{
    return QStringLiteral("id,name,parentReference,file,folder,root,deleted,size,eTag,cTag,lastModifiedDateTime,fileSystemInfo,remoteItem,lastModifiedBy");
}

bool DriveItem::isSafeName(const QString &name)
{
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..") && !name.contains(QLatin1Char('/'))
        && !name.contains(QLatin1Char('\\')) && !name.contains(QChar(0)) && name.toUtf8().size() <= 255;
}

bool DriveItem::isSafeId(const QString &id)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9!._-]{1,256}$"));
    return safe.match(id).hasMatch();
}

}
