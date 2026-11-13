// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config.h"

#include <QList>
#include <QString>

namespace GraphSync
{

class GraphClient;

// PC folder backup (the Windows client's "Known Folder Move"): Desktop, Documents and Pictures of
// this computer kept in the account's OneDrive, in its folders of the same names as on Windows. The
// local folders stay where they are; each becomes a location of its own (kind "folder") synced with
// that OneDrive folder, and the account's OneDrive location leaves that folder out so nothing is
// synced twice. Stopping a backup only removes the location: the files stay here and online.
namespace FolderBackup
{

struct Folder {
    QString key; // "desktop", "documents", "pictures"
    QString onlineName; // the folder in OneDrive: "Desktop", "Documents", "Pictures"
    QString title; // translated, for the UI
    QString localPath; // this computer's folder (XDG user directory)
};

// The three folders with this computer's paths
QList<Folder> folders();
std::optional<Folder> folder(const QString &key);

// Why localPath cannot be backed up (translated), or empty when it can: it must be a folder of its
// own (not the home folder) that is not inside, around or the same as a location, also through
// symbolic links
QString problem(const QString &localPath, const QList<RootConfig> &roots);

// The location for backing up folder with account's OneDrive: finds the OneDrive and its folder of
// that name, creating the folder when it is not there. Nothing is changed locally. onlineName gets
// the folder's name as OneDrive spells it (what the OneDrive location leaves out). On failure the
// result has no driveId and error says why.
RootConfig prepare(GraphClient *graph, const QString &accountId, const Folder &folder, QString *onlineName, QString *error);

// The account's OneDrive location (kind "personal"), if this computer syncs it
std::optional<RootConfig> personalRoot(const QString &accountId, const QList<RootConfig> &roots);

// Starts backing up folder with the account's OneDrive (the app and graphsyncctl both use this):
// checks the local folder, finds or creates the online one, has the OneDrive location leave it out
// and adds the location through the running sync service. Returns the new location's id, or empty
// with error set; nothing is changed when the checks or the online part fail.
QString start(GraphClient *graph, const QString &accountId, const Folder &folder, QString *error);

}

}
