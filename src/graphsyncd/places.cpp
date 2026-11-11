// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "places.h"

#include <KBookmarkManager>
#include <KConfigGroup>
#include <KSharedConfig>

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QUrl>

namespace GraphSync::Places
{

namespace
{
// Marks the entries we own, so we never touch the user's own places
const QString ownerKey = QStringLiteral("PlasmaGraphsyncLinux");
// Entries made by plasma-onedrive-linux, the name of this client before 0.7.0, are ours too
const QString legacyOwnerKey = QStringLiteral("PlasmaOnedriveLinux");

bool isOwn(const KBookmark &bookmark)
{
    return !bookmark.metaDataItem(ownerKey).isEmpty() || !bookmark.metaDataItem(legacyOwnerKey).isEmpty();
}

QString placesFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/user-places.xbel");
}

KConfigGroup addedGroup()
{
    return KSharedConfig::openConfig(QStringLiteral("graphsyncrc"))->group(QStringLiteral("Places"));
}

KBookmark find(const KBookmarkGroup &root, const QUrl &url, bool ownOnly)
{
    for (KBookmark bookmark = root.first(); !bookmark.isNull(); bookmark = root.next(bookmark)) {
        if (bookmark.url() == url && (!ownOnly || isOwn(bookmark))) {
            return bookmark;
        }
    }
    return {};
}
}

void ensure(const QString &path, const QString &label, const QString &iconName)
{
    const QString cleanPath = QDir::cleanPath(path);
    KConfigGroup added = addedGroup();
    QStringList paths = added.readEntry("Added", QStringList());
    if (paths.contains(cleanPath)) {
        return;
    }

    KBookmarkManager manager(placesFile());
    KBookmarkGroup root = manager.root();
    const QUrl url = QUrl::fromLocalFile(cleanPath);
    if (find(root, url, false).isNull()) {
        KBookmark bookmark = root.addBookmark(label, url, iconName);
        // Same ID scheme as KFilePlacesModel
        static int count = 0;
        bookmark.setMetaDataItem(QStringLiteral("ID"), QStringLiteral("%1/%2").arg(QDateTime::currentSecsSinceEpoch()).arg(count++));
        bookmark.setMetaDataItem(ownerKey, QStringLiteral("1"));

        // Right below Home and our earlier entries, as Windows lists OneDrive near the top
        KBookmark after = find(root, QUrl::fromLocalFile(QDir::homePath()), false);
        for (KBookmark other = root.first(); !other.isNull(); other = root.next(other)) {
            if (isOwn(other) && other.url() != url) {
                after = other;
            }
        }
        if (!after.isNull()) {
            root.moveBookmark(bookmark, after);
        }
        manager.save();
        manager.emitChanged(root);
    }

    paths.append(cleanPath);
    added.writeEntry("Added", paths);
    added.sync();
}

void remove(const QString &path)
{
    const QString cleanPath = QDir::cleanPath(path);
    KConfigGroup added = addedGroup();
    QStringList paths = added.readEntry("Added", QStringList());
    if (paths.removeAll(cleanPath)) {
        added.writeEntry("Added", paths);
        added.sync();
    }

    KBookmarkManager manager(placesFile());
    KBookmarkGroup root = manager.root();
    const KBookmark bookmark = find(root, QUrl::fromLocalFile(cleanPath), true);
    if (!bookmark.isNull()) {
        root.deleteBookmark(bookmark);
        manager.save();
        manager.emitChanged(root);
    }
}

}
