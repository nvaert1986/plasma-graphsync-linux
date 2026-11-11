// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace GraphSync
{

class GraphClient;
struct GraphResponse;

// Copy link, Share and Version history: Graph calls on one synced file or folder.
// Items are addressed by their path below the sync root, so this works from any process
// without the root's state database. All calls block (GraphClient's local event loop).
// Errors are user-facing sentences in *error.
class ItemService
{
public:
    struct Item {
        QString driveId;
        QString id;
        QString name;
        QString webUrl;
        // SharePoint's own permissions page for this item (site owners can manage access there)
        QString permissionsUrl;
        bool isFolder = false;
        bool valid() const
        {
            return !id.isEmpty();
        }
    };

    explicit ItemService(GraphClient *graph);

    // The online item behind a local path inside a sync root
    Item resolve(const QString &localPath, QString *error);

    // Sharing link. Empty scope = the organisation's default link (what OneDrive's "Copy link" does).
    // type: "view" or "edit"; scope: "", "anonymous", "organization" or "users".
    // Returns {url, scope, type}.
    QVariantMap createLink(const Item &item, const QString &type, const QString &scope, const QDateTime &expiry, QString *error);

    // Who has access: [{id, kind (link|person|group), name, detail, role (owner|write|read),
    // scope, url, expires, inherited, removable}]; only links are removable
    QVariantList permissions(const Item &item, QString *error);
    bool removePermission(const Item &item, const QString &permissionId, QString *error);

    // Gives people access by e-mail address. role: "read" or "write"
    bool invite(const Item &item, const QStringList &emails, const QString &role, bool sendInvitation, const QString &message, QString *error);

    // Name suggestions: [{name, email}]. Needs People.Read or User.ReadBasic.All; without them
    // *notAllowed is set and the caller asks for plain e-mail addresses instead.
    QVariantList searchPeople(const QString &query, bool *notAllowed, QString *error);

    // Newest first: [{id, modified (QDateTime), modifiedBy, size, current}]
    QVariantList versions(const Item &item, QString *error);
    bool downloadVersion(const Item &item, const QString &versionId, const QString &targetPath, QString *error);
    bool restoreVersion(const Item &item, const QString &versionId, QString *error);

    // "Add shortcut to My files" for a folder: a shortcut item in the user's own OneDrive root
    // that points to this folder. Returns the shortcut's name (Graph adds a number on a clash).
    QString addShortcut(const Item &folder, QString *error);

private:
    static QString itemPath(const Item &item);
    static QString describe(const GraphResponse &response);

    GraphClient *m_graph;
};

}
