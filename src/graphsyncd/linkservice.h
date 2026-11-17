// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace GraphSync
{

class Auth;

// What other apps of the suite ask Graph Sync about synced files (phase 10 step 7, Graph Mail):
// a file's online address, sharing it with the recipients of a message, and the local copy of a
// SharePoint or OneDrive link. Each call blocks (Graph requests): run it off the main thread.
// Answers are JSON objects with "error" on failure; "notSynced" says the file is not online yet
// (just copied into a synced folder), so the caller tries again.
namespace LinkService
{
using Auths = QHash<QString, std::shared_ptr<Auth>>;

// {url, name, accountId, synced}: url is the file's own address in OneDrive/SharePoint (works for
// the people it is shared with)
QJsonObject fileLink(const QString &localPath, const Auths &auths);

// Gives the people with these addresses access (role "write": can edit, Outlook's default for a
// file shared from a message; "read": can view), without an invitation mail: the message is the
// invitation. {url, name}
QJsonObject shareFile(const QString &localPath, const QStringList &emails, const QString &role, const Auths &auths);

// {path, placeholder}: the local copy of a SharePoint/OneDrive link when a synced location holds
// it (placeholder: only the online-only placeholder is on disk); {} when none does
QJsonObject localPathForUrl(const QString &url, const Auths &auths);
}

}
