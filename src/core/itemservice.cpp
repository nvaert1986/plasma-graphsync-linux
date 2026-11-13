// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "itemservice.h"
#include "statedb.h"
#include "auth.h"
#include "config.h"
#include "graphclient.h"
#include "placeholder.h"

#include <KLocalizedString>

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

#include <algorithm>

namespace GraphSync
{

namespace
{
QString encodePath(const QString &rel)
{
    QStringList parts;
    const QStringList names = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &name : names) {
        parts.append(QString::fromLatin1(QUrl::toPercentEncoding(name)));
    }
    return parts.join(QLatin1Char('/'));
}

// Every page of a list into *all: an item can have more sharing links or versions than one page
// holds. False with the failing page's response in *failed.
bool allPages(GraphClient *graph, const QString &path, QJsonArray *all, GraphResponse *failed)
{
    QString next = path;
    for (int page = 0; !next.isEmpty() && page < 100; ++page) {
        const GraphResponse response = graph->get(next);
        if (!response.ok()) {
            *failed = response;
            return false;
        }
        const QJsonObject json = response.json();
        const QJsonArray values = json.value(QLatin1String("value")).toArray();
        for (const QJsonValue &value : values) {
            all->append(value);
        }
        next = json.value(QLatin1String("@odata.nextLink")).toString();
    }
    return true;
}

QString displayNameOf(const QJsonObject &identitySet)
{
    for (const char *key : {"user", "siteUser", "group", "siteGroup", "application"}) {
        const QJsonObject identity = identitySet.value(QLatin1String(key)).toObject();
        if (!identity.isEmpty()) {
            const QString name = identity.value(QLatin1String("displayName")).toString();
            return name.isEmpty() ? identity.value(QLatin1String("email")).toString() : name;
        }
    }
    return {};
}

QString emailOf(const QJsonObject &identitySet)
{
    for (const char *key : {"user", "siteUser"}) {
        const QJsonObject identity = identitySet.value(QLatin1String(key)).toObject();
        const QString email = identity.value(QLatin1String("email")).toString();
        if (!email.isEmpty()) {
            return email;
        }
    }
    return {};
}

// The strongest role in a Graph roles array
QString roleOf(const QJsonArray &roles)
{
    QString role = QStringLiteral("read");
    for (const QJsonValue &value : roles) {
        const QString name = value.toString().toLower();
        if (name == QLatin1String("owner") || name.contains(QLatin1String("full control"))) {
            return QStringLiteral("owner");
        }
        if (name == QLatin1String("write") || name.contains(QLatin1String("edit")) || name.contains(QLatin1String("contribute"))) {
            role = QStringLiteral("write");
        }
    }
    return role;
}
}

ItemService::ItemService(GraphClient *graph)
    : m_graph(graph)
    // Without a sign-in: requests fail with "not signed in" (an account that is gone)
    , m_noAccountAuth(std::make_unique<Auth>(AuthConfig{}))
    , m_noAccountGraph(std::make_unique<GraphClient>(m_noAccountAuth.get()))
{
}

ItemService::~ItemService() = default;

void ItemService::setGraphProvider(GraphProvider provider)
{
    m_provider = std::move(provider);
}

GraphClient *ItemService::graphFor(const QString &accountId) const
{
    if (accountId.isEmpty() || !m_provider) {
        return m_graph;
    }
    // Never another account's client for an account that is gone
    GraphClient *graph = m_provider(accountId);
    return graph ? graph : m_noAccountGraph.get();
}

QString ItemService::itemPath(const Item &item)
{
    return QStringLiteral("/drives/%1/items/%2").arg(item.driveId, item.id);
}

QString ItemService::describe(const GraphResponse &response)
{
    const QString code = response.graphErrorCode();
    if (response.status == 0) {
        return i18n("Could not reach Microsoft 365. Check the network connection.");
    }
    if (response.status == 404 || code == QLatin1String("itemNotFound")) {
        return i18n("This item is not online yet. Wait until it has synced and try again.");
    }
    if (code == QLatin1String("notAllowed") || code == QLatin1String("accessDenied") || response.status == 403) {
        return i18n("Your organisation does not allow this here. (%1)", response.describe());
    }
    return response.describe();
}

ItemService::Item ItemService::resolve(const QString &localPath, QString *error)
{
    // A placeholder (Files On-Demand) stands for its real file
    const QString path = Placeholder::hasSuffix(localPath) && Placeholder::isPlaceholder(localPath) ? Placeholder::realPathOf(QDir::cleanPath(localPath))
                                                                                                   : QDir::cleanPath(localPath);
    const RootConfig owner = Config::rootForPath(path);
    if (owner.id.isEmpty()) {
        *error = i18n("%1 is not in a synced folder.", path);
        return {};
    }
    // Only with the location's own account: another one (the current account, say an
    // administrator) must not share or change permissions in its name
    if (!GraphCore::Accounts::find(owner.accountId).isValid()) {
        *error = i18n("The account of %1 is no longer on this computer. Sign in to it again in Settings > Accounts.", owner.name);
        return {};
    }

    const QString rel = encodePath(path.mid(owner.localPath.size() + 1));
    const QString base = owner.remoteRootId.isEmpty() ? QStringLiteral("/drives/%1/root").arg(owner.driveId)
                                                      : QStringLiteral("/drives/%1/items/%2").arg(owner.driveId, owner.remoteRootId);
    const QString request = (rel.isEmpty() ? base : base + QStringLiteral(":/") + rel + QLatin1Char(':'))
        + QStringLiteral("?$select=id,name,webUrl,folder,file,parentReference,sharepointIds");
    const GraphResponse response = graphFor(owner.accountId)->get(request);
    if (!response.ok()) {
        *error = describe(response);
        return {};
    }
    const QJsonObject json = response.json();
    Item item;
    item.driveId = json.value(QLatin1String("parentReference")).toObject().value(QLatin1String("driveId")).toString(owner.driveId);
    item.id = json.value(QLatin1String("id")).toString();
    item.name = json.value(QLatin1String("name")).toString();
    item.webUrl = json.value(QLatin1String("webUrl")).toString();
    item.isFolder = json.contains(QLatin1String("folder"));
    item.accountId = owner.accountId;
    // The location's root itself always is; anything else must be what the sync state knows
    // under that path (a file renamed or replaced here and not uploaded yet resolves by its path
    // to the old online item)
    item.matchesLocal = rel.isEmpty() || StateDb::localMatches(owner.id, item.id, path);

    // The classic permissions page: <site>/_layouts/15/user.aspx?obj={list},<item>,LISTITEM&List={list}.
    // The library root has no list item and uses the library form (obj={list},doclib).
    const QJsonObject ids = json.value(QLatin1String("sharepointIds")).toObject();
    const QString siteUrl = ids.value(QLatin1String("siteUrl")).toString();
    const QString listId = ids.value(QLatin1String("listId")).toString();
    if (!siteUrl.isEmpty() && !listId.isEmpty()) {
        const QString list = QStringLiteral("%7B") + listId + QStringLiteral("%7D");
        const QString listItemId = ids.value(QLatin1String("listItemId")).toString();
        const QString object = listItemId.isEmpty() ? QString(list + QStringLiteral(",doclib")) : QString(list + QLatin1Char(',') + listItemId + QStringLiteral(",LISTITEM"));
        item.permissionsUrl = siteUrl + QStringLiteral("/_layouts/15/user.aspx?obj=") + object + QStringLiteral("&List=") + list;
    }
    return item;
}

QString ItemService::notSyncedYet(const Item &item)
{
    return i18n("%1 is new or renamed here and not online yet; try again once it is synced.", item.name);
}

QVariantMap ItemService::createLink(const Item &item, const QString &type, const QString &scope, const QDateTime &expiry, QString *error)
{
    if (!item.matchesLocal) {
        *error = notSyncedYet(item);
        return {};
    }
    QJsonObject body{{QStringLiteral("type"), type}};
    if (!scope.isEmpty()) {
        body.insert(QStringLiteral("scope"), scope);
    }
    if (expiry.isValid()) {
        body.insert(QStringLiteral("expirationDateTime"), expiry.toUTC().toString(Qt::ISODate));
    }
    // Never send retainInheritedPermissions: false, it would drop all existing access on first share
    const GraphResponse response = graphFor(item.accountId)->postJson(itemPath(item) + QStringLiteral("/createLink"), body);
    if (!response.ok()) {
        *error = describe(response);
        return {};
    }
    const QJsonObject link = response.json().value(QLatin1String("link")).toObject();
    return {
        {QStringLiteral("url"), link.value(QLatin1String("webUrl")).toString()},
        {QStringLiteral("scope"), link.value(QLatin1String("scope")).toString()},
        {QStringLiteral("type"), link.value(QLatin1String("type")).toString()},
    };
}

QVariantList ItemService::permissions(const Item &item, QString *error)
{
    QJsonArray list;
    GraphResponse failed;
    if (!allPages(graphFor(item.accountId), itemPath(item) + QStringLiteral("/permissions"), &list, &failed)) {
        *error = describe(failed);
        return {};
    }
    QVariantList result;
    for (const QJsonValue &value : list) {
        const QJsonObject permission = value.toObject();
        const QJsonObject link = permission.value(QLatin1String("link")).toObject();
        const QString role = roleOf(permission.value(QLatin1String("roles")).toArray());
        const bool inherited = !permission.value(QLatin1String("inheritedFrom")).toObject().isEmpty();
        // Only sharing links can be removed here. SharePoint reports the site groups (Owners,
        // Members, Visitors) as direct permissions on every item; removing one would break
        // inheritance and lock the team out. People and groups are managed in the browser.
        QVariantMap entry{
            {QStringLiteral("id"), permission.value(QLatin1String("id")).toString()},
            {QStringLiteral("role"), role},
            {QStringLiteral("inherited"), inherited},
            {QStringLiteral("removable"), !inherited && !link.isEmpty()},
        };
        if (!link.isEmpty()) {
            QStringList people;
            const QJsonArray granted = permission.value(QLatin1String("grantedToIdentitiesV2")).toArray();
            for (const QJsonValue &identity : granted) {
                const QString name = displayNameOf(identity.toObject());
                if (!name.isEmpty()) {
                    people.append(name);
                }
            }
            entry.insert(QStringLiteral("kind"), QStringLiteral("link"));
            entry.insert(QStringLiteral("scope"), link.value(QLatin1String("scope")).toString());
            entry.insert(QStringLiteral("url"), link.value(QLatin1String("webUrl")).toString());
            entry.insert(QStringLiteral("detail"), people.join(QStringLiteral(", ")));
            entry.insert(QStringLiteral("expires"), QDateTime::fromString(permission.value(QLatin1String("expirationDateTime")).toString(), Qt::ISODate));
            // A link's role follows its type
            entry.insert(QStringLiteral("role"), link.value(QLatin1String("type")).toString() == QLatin1String("edit") ? QStringLiteral("write") : QStringLiteral("read"));
        } else {
            const QJsonObject granted = permission.value(QLatin1String("grantedToV2")).toObject();
            const bool isGroup = granted.contains(QLatin1String("group")) || granted.contains(QLatin1String("siteGroup"));
            entry.insert(QStringLiteral("kind"), isGroup ? QStringLiteral("group") : QStringLiteral("person"));
            entry.insert(QStringLiteral("name"), displayNameOf(granted));
            entry.insert(QStringLiteral("detail"), emailOf(granted));
            // A site group and the Microsoft 365 group behind it come as two entries with one name
            bool duplicate = false;
            for (const QVariant &existing : std::as_const(result)) {
                const QVariantMap other = existing.toMap();
                if (other.value(QStringLiteral("kind")) == entry.value(QStringLiteral("kind")) && other.value(QStringLiteral("name")) == entry.value(QStringLiteral("name"))
                    && other.value(QStringLiteral("role")) == role) {
                    duplicate = true;
                }
            }
            if (duplicate) {
                continue;
            }
        }
        result.append(entry);
    }
    // Owners first, then people and groups, then links
    std::stable_sort(result.begin(), result.end(), [](const QVariant &a, const QVariant &b) {
        auto rank = [](const QVariantMap &entry) {
            if (entry.value(QStringLiteral("role")) == QLatin1String("owner")) {
                return 0;
            }
            return entry.value(QStringLiteral("kind")) == QLatin1String("link") ? 2 : 1;
        };
        return rank(a.toMap()) < rank(b.toMap());
    });
    return result;
}

bool ItemService::removePermission(const Item &item, const QString &permissionId, QString *error)
{
    if (!item.matchesLocal) {
        *error = notSyncedYet(item);
        return false;
    }
    const GraphResponse response = graphFor(item.accountId)->deleteResource(itemPath(item) + QStringLiteral("/permissions/") + permissionId);
    if (!response.ok()) {
        *error = describe(response);
        return false;
    }
    return true;
}

bool ItemService::invite(const Item &item, const QStringList &emails, const QString &role, bool sendInvitation, const QString &message, QString *error)
{
    if (!item.matchesLocal) {
        *error = notSyncedYet(item);
        return false;
    }
    QJsonArray recipients;
    for (const QString &email : emails) {
        recipients.append(QJsonObject{{QStringLiteral("email"), email.trimmed()}});
    }
    QJsonObject body{
        {QStringLiteral("recipients"), recipients},
        {QStringLiteral("roles"), QJsonArray{role}},
        {QStringLiteral("requireSignIn"), true},
        {QStringLiteral("sendInvitation"), sendInvitation},
    };
    if (sendInvitation && !message.trimmed().isEmpty()) {
        body.insert(QStringLiteral("message"), message.trimmed());
    }
    const GraphResponse response = graphFor(item.accountId)->postJson(itemPath(item) + QStringLiteral("/invite"), body);
    if (!response.ok()) {
        *error = describe(response);
        return false;
    }
    return true;
}

QVariantList ItemService::searchPeople(const QString &query, const QString &accountId, bool *notAllowed, QString *error)
{
    *notAllowed = false;
    const QString trimmed = query.trimmed();
    if (trimmed.size() < 2) {
        return {};
    }
    QVariantList result;
    auto add = [&result](const QString &name, const QString &email) {
        if (email.isEmpty()) {
            return;
        }
        for (const QVariant &existing : std::as_const(result)) {
            if (existing.toMap().value(QStringLiteral("email")).toString().compare(email, Qt::CaseInsensitive) == 0) {
                return;
            }
        }
        result.append(QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("email"), email}});
    };

    // People the user works with (People.Read), then the directory (User.ReadBasic.All)
    const QString search = QString::fromLatin1(QUrl::toPercentEncoding(QLatin1Char('"') + trimmed + QLatin1Char('"')));
    GraphResponse response = graphFor(accountId)->get(QStringLiteral("/me/people?$search=%1&$top=8&$select=displayName,scoredEmailAddresses").arg(search));
    if (response.ok()) {
        const QJsonArray people = response.json().value(QLatin1String("value")).toArray();
        for (const QJsonValue &value : people) {
            const QJsonObject person = value.toObject();
            const QJsonArray addresses = person.value(QLatin1String("scoredEmailAddresses")).toArray();
            add(person.value(QLatin1String("displayName")).toString(), addresses.isEmpty() ? QString() : addresses.first().toObject().value(QLatin1String("address")).toString());
        }
    }
    const bool peopleRefused = response.status == 401 || response.status == 403;

    QString escaped = trimmed;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    const QString filter = QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("startswith(displayName,'%1') or startswith(mail,'%1')").arg(escaped)));
    response = graphFor(accountId)->get(QStringLiteral("/users?$filter=%1&$top=8&$select=displayName,mail").arg(filter));
    if (response.ok()) {
        const QJsonArray users = response.json().value(QLatin1String("value")).toArray();
        for (const QJsonValue &value : users) {
            const QJsonObject user = value.toObject();
            add(user.value(QLatin1String("displayName")).toString(), user.value(QLatin1String("mail")).toString());
        }
    } else if (peopleRefused && (response.status == 401 || response.status == 403)) {
        *notAllowed = true;
    } else if (result.isEmpty() && !peopleRefused) {
        *error = describe(response);
    }
    return result;
}

QVariantList ItemService::versions(const Item &item, QString *error)
{
    QJsonArray list;
    GraphResponse failed;
    if (!allPages(graphFor(item.accountId), itemPath(item) + QStringLiteral("/versions"), &list, &failed)) {
        *error = describe(failed);
        return {};
    }
    QVariantList result;
    for (const QJsonValue &value : list) {
        const QJsonObject version = value.toObject();
        result.append(QVariantMap{
            {QStringLiteral("id"), version.value(QLatin1String("id")).toString()},
            {QStringLiteral("modified"), QDateTime::fromString(version.value(QLatin1String("lastModifiedDateTime")).toString(), Qt::ISODate)},
            {QStringLiteral("modifiedBy"), displayNameOf(version.value(QLatin1String("lastModifiedBy")).toObject())},
            {QStringLiteral("size"), version.value(QLatin1String("size")).toDouble()},
        });
    }
    std::stable_sort(result.begin(), result.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("modified")).toDateTime() > b.toMap().value(QStringLiteral("modified")).toDateTime();
    });
    for (qsizetype i = 0; i < result.size(); ++i) {
        QVariantMap version = result.at(i).toMap();
        version.insert(QStringLiteral("current"), i == 0);
        result[i] = version;
    }
    return result;
}

bool ItemService::downloadVersion(const Item &item, const QString &versionId, const QString &targetPath, QString *error)
{
    QSaveFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = i18n("Cannot write %1: %2", targetPath, file.errorString());
        return false;
    }
    const GraphResponse response = graphFor(item.accountId)->download(itemPath(item) + QStringLiteral("/versions/%1/content").arg(versionId), &file);
    if (!response.ok()) {
        file.cancelWriting();
        *error = describe(response);
        return false;
    }
    if (!file.commit()) {
        *error = i18n("Cannot write %1: %2", targetPath, file.errorString());
        return false;
    }
    return true;
}

QString ItemService::addShortcut(const Item &folder, QString *error)
{
    const QJsonObject body{
        {QStringLiteral("name"), folder.name},
        {QStringLiteral("remoteItem"),
         QJsonObject{{QStringLiteral("id"), folder.id}, {QStringLiteral("parentReference"), QJsonObject{{QStringLiteral("driveId"), folder.driveId}}}}},
        {QStringLiteral("@microsoft.graph.conflictBehavior"), QStringLiteral("rename")},
    };
    const GraphResponse response = graphFor(folder.accountId)->postJson(QStringLiteral("/me/drive/root/children"), body);
    if (!response.ok()) {
        *error = describe(response);
        return {};
    }
    return response.json().value(QLatin1String("name")).toString();
}

bool ItemService::restoreVersion(const Item &item, const QString &versionId, QString *error)
{
    if (!item.matchesLocal) {
        *error = notSyncedYet(item);
        return false;
    }
    const GraphResponse response = graphFor(item.accountId)->postJson(itemPath(item) + QStringLiteral("/versions/%1/restoreVersion").arg(versionId), {});
    if (!response.ok()) {
        *error = describe(response);
        return false;
    }
    return true;
}

}
