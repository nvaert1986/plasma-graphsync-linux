// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace GraphSync
{

// The parts of a Graph driveItem the sync engine needs.
struct DriveItem {
    QString id;
    QString parentId;
    QString name;
    bool isFolder = false;
    bool isRoot = false;
    bool deleted = false;
    // "Add shortcut to My files" items point into another drive (remoteItem facet)
    bool isRemoteShortcut = false;
    // Where a shortcut points: the drive and item it stands for
    QString remoteDriveId;
    QString remoteId;
    QString remoteWebUrl;
    bool remoteIsFolder = false;
    // Who changed it last (lastModifiedBy.user)
    QString modifiedById;
    QString modifiedByName;
    qint64 size = 0;
    QString eTag;
    QString cTag;
    QString quickXorHash;
    QDateTime modified;

    static DriveItem fromJson(const QJsonObject &json);
    // Microsoft never sends names with a slash, NUL, "." or ".." or ids with odd characters, but a
    // tampered answer must not be able to build a path outside the sync folder
    static bool isSafeName(const QString &name);
    static bool isSafeId(const QString &id);

    // Fields to request with $select on delta and item calls
    static QString selectFields();
};

}
