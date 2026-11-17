// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "linkservice.h"
#include "auth.h"
#include "config.h"
#include "graphclient.h"
#include "itemservice.h"
#include "placeholder.h"
#include "statedb.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

#include <map>

namespace GraphSync::LinkService
{

namespace
{
// An ItemService whose Graph clients live on this (the calling) thread, one per account
struct Items {
    explicit Items(const Auths &auths)
        : service(nullptr)
    {
        service.setGraphProvider([this, auths](const QString &accountId) -> GraphClient * {
            auto it = clients.find(accountId);
            if (it == clients.end()) {
                const std::shared_ptr<Auth> auth = auths.value(accountId);
                if (!auth) {
                    return nullptr;
                }
                it = clients.emplace(accountId, std::make_unique<GraphClient>(auth.get())).first;
            }
            return it->second.get();
        });
    }
    std::map<QString, std::unique_ptr<GraphClient>> clients;
    ItemService service;
};

QJsonObject failure(const QString &error, bool notSynced = false)
{
    QJsonObject answer{{QStringLiteral("error"), error}};
    if (notSynced) {
        answer.insert(QStringLiteral("notSynced"), true);
    }
    return answer;
}

// A file just put into a synced folder is not online yet: Graph knows nothing under its path. The
// error is a (translated) sentence, so the test is the file itself: it is here, Graph does not have it
bool looksNotUploaded(const QString &localPath)
{
    return QFileInfo::exists(localPath);
}

// Graph's id for a sharing URL: "u!" + the URL in unpadded base64url
QString shareIdOf(const QString &url)
{
    return QStringLiteral("u!") + QString::fromLatin1(url.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
}

QJsonObject fileLink(const QString &localPath, const Auths &auths)
{
    Items items(auths);
    QString error;
    const ItemService::Item item = items.service.resolve(localPath, &error);
    if (!item.valid()) {
        return failure(error, looksNotUploaded(localPath));
    }
    return {{QStringLiteral("url"), item.webUrl},
            {QStringLiteral("name"), item.name},
            {QStringLiteral("accountId"), item.accountId},
            {QStringLiteral("synced"), item.matchesLocal}};
}

QJsonObject shareFile(const QString &localPath, const QStringList &emails, const QString &role, const Auths &auths)
{
    if (emails.isEmpty()) {
        return failure(QStringLiteral("no recipients"));
    }
    Items items(auths);
    QString error;
    const ItemService::Item item = items.service.resolve(localPath, &error);
    if (!item.valid()) {
        return failure(error, looksNotUploaded(localPath));
    }
    if (!item.matchesLocal) {
        return failure(ItemService::notSyncedYet(item), true);
    }
    if (!items.service.invite(item, emails, role == QLatin1String("read") ? QStringLiteral("read") : QStringLiteral("write"), false, {}, &error)) {
        return failure(error);
    }
    return {{QStringLiteral("url"), item.webUrl}, {QStringLiteral("name"), item.name}};
}

QJsonObject localPathForUrl(const QString &url, const Auths &auths)
{
    const QUrl parsed(url);
    if (!parsed.isValid() || parsed.scheme() != QLatin1String("https")) {
        return {};
    }
    const QList<RootConfig> roots = Config::roots();
    // The accounts with a location here, asked one after another (a link of another organisation
    // is unknown to all of them)
    QStringList accounts;
    for (const RootConfig &root : roots) {
        if (!accounts.contains(root.accountId) && auths.contains(root.accountId)) {
            accounts.append(root.accountId);
        }
    }
    for (const QString &accountId : std::as_const(accounts)) {
        GraphClient graph(auths.value(accountId).get());
        const GraphResponse response = graph.get(QStringLiteral("/shares/%1/driveItem?$select=id,parentReference").arg(shareIdOf(url)));
        if (!response.ok()) {
            continue;
        }
        const QJsonObject item = response.json();
        const QString itemId = item.value(QLatin1String("id")).toString();
        const QString driveId = item.value(QLatin1String("parentReference")).toObject().value(QLatin1String("driveId")).toString();
        for (const RootConfig &root : roots) {
            if (root.accountId != accountId || root.driveId.compare(driveId, Qt::CaseInsensitive) != 0) {
                continue;
            }
            bool dehydrated = false;
            const QString rel = StateDb::relativePathOf(root.id, itemId, root.remoteRootId, &dehydrated);
            if (rel.isEmpty()) {
                continue;
            }
            const QString path = QDir(root.localPath).filePath(rel);
            // Only what is really there (the state could be behind the disk)
            if (dehydrated && QFileInfo::exists(Placeholder::pathFor(path))) {
                return {{QStringLiteral("path"), Placeholder::pathFor(path)}, {QStringLiteral("placeholder"), true}};
            }
            if (QFileInfo::exists(path)) {
                return {{QStringLiteral("path"), path}, {QStringLiteral("placeholder"), false}};
            }
        }
    }
    return {};
}

}
