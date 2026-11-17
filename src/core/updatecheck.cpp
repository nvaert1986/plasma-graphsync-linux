// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "updatecheck.h"
#include "config.h"
#include "graphsync_debug.h"

#include <KConfig>
#include <KConfigGroup>

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QVersionNumber>

namespace GraphSync
{

namespace
{
const QString releasesPage = QStringLiteral("https://github.com/nvaert1986/plasma-graphsync-linux/releases/tag/v");
const QString latestApi = QStringLiteral("https://api.github.com/repos/nvaert1986/plasma-graphsync-linux/releases/latest");
constexpr qint64 checkEverySecs = 24 * 3600;
// A little under a day: the daily check runs on an hourly timer
constexpr qint64 remindEverySecs = 23 * 3600;
constexpr int timeoutMs = 30 * 1000;

KConfigGroup state(KConfig &config)
{
    return config.group(QStringLiteral("Update"));
}

QString stateFile()
{
    return QDir(Config::stateDirectory()).filePath(QStringLiteral("updatecheck"));
}

QUrl latestUrl()
{
#ifdef GRAPHSYNC_TESTING
    // Tests only (Debug builds of this project; never in a package): ask a local server instead
    const QString test = qEnvironmentVariable("GRAPHSYNC_TEST_UPDATE_URL");
    if (!test.isEmpty()) {
        return QUrl(test);
    }
#endif
    return QUrl(latestApi);
}
}

namespace UpdateCheck
{
bool compiledIn()
{
#ifdef GRAPHSYNC_UPDATE_CHECK
    return true;
#else
    return false;
#endif
}

QString cleanVersion(const QString &tag)
{
    static const QRegularExpression plain(QStringLiteral("^v?(\\d{1,4}(?:\\.\\d{1,4}){1,3})$"));
    const QRegularExpressionMatch match = plain.match(tag.trimmed());
    return match.hasMatch() ? match.captured(1) : QString();
}

std::optional<Release> parseLatest(const QByteArray &json)
{
    const QJsonObject release = QJsonDocument::fromJson(json).object();
    if (release.value(QLatin1String("draft")).toBool() || release.value(QLatin1String("prerelease")).toBool()) {
        return std::nullopt;
    }
    // Only a plain version; the page is made here, not taken from the answer
    const QString version = cleanVersion(release.value(QLatin1String("tag_name")).toString());
    if (version.isEmpty()) {
        return std::nullopt;
    }
    return Release{version, QUrl(releasesPage + version)};
}

bool isNewer(const QString &candidate, const QString &current)
{
    const QString a = cleanVersion(candidate);
    const QString b = cleanVersion(current);
    return !a.isEmpty() && !b.isEmpty() && QVersionNumber::compare(QVersionNumber::fromString(a), QVersionNumber::fromString(b)) > 0;
}

QString currentVersion()
{
#ifdef GRAPHSYNC_TESTING
    // Tests only: pretend to be an older version
    const QString test = qEnvironmentVariable("GRAPHSYNC_TEST_CURRENT_VERSION");
    if (!test.isEmpty()) {
        return test;
    }
#endif
    return QStringLiteral(GRAPHSYNC_VERSION);
}
}

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

void UpdateChecker::check(bool force)
{
    if (m_busy) {
        return;
    }
    {
        KConfig config(stateFile(), KConfig::SimpleConfig);
        const QDateTime last = QDateTime::fromString(state(config).readEntry("LastCheck", QString()), Qt::ISODate);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (!force && last.isValid() && last <= now && last.secsTo(now) < checkEverySecs) {
            Q_EMIT checked(true, {});
            return;
        }
    }
    m_busy = true;
    QNetworkRequest request(latestUrl());
    // GitHub refuses requests without a User-Agent
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("plasma-graphsync-linux/%1").arg(QStringLiteral(GRAPHSYNC_VERSION)));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(timeoutMs);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_busy = false;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            const QString error = reply->error() != QNetworkReply::NoError ? reply->errorString() : QStringLiteral("HTTP %1").arg(status);
            qCInfo(GRAPHSYNC) << "Could not look for a new version:" << error;
            Q_EMIT checked(false, error);
            return;
        }
        const std::optional<UpdateCheck::Release> release = UpdateCheck::parseLatest(reply->readAll());
        if (!release) {
            qCInfo(GRAPHSYNC) << "Could not look for a new version: unexpected answer";
            Q_EMIT checked(false, QStringLiteral("unexpected answer"));
            return;
        }
        KConfig config(stateFile(), KConfig::SimpleConfig);
        KConfigGroup group = state(config);
        group.writeEntry("LastCheck", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        group.writeEntry("Latest", release->version);
        config.sync();
        qCInfo(GRAPHSYNC) << "Newest release:" << release->version;
        Q_EMIT checked(true, {});
    });
}

QString UpdateChecker::available() const
{
    KConfig config(stateFile(), KConfig::SimpleConfig);
    const KConfigGroup group = state(config);
    const QString latest = UpdateCheck::cleanVersion(group.readEntry("Latest", QString()));
    if (!UpdateCheck::isNewer(latest, UpdateCheck::currentVersion()) || latest == group.readEntry("Skipped", QString())) {
        return {};
    }
    return latest;
}

QUrl UpdateChecker::page() const
{
    const QString version = available();
    return version.isEmpty() ? QUrl() : QUrl(releasesPage + version);
}

QString UpdateChecker::latest() const
{
    KConfig config(stateFile(), KConfig::SimpleConfig);
    return UpdateCheck::cleanVersion(state(config).readEntry("Latest", QString()));
}

QDateTime UpdateChecker::lastCheck() const
{
    KConfig config(stateFile(), KConfig::SimpleConfig);
    return QDateTime::fromString(state(config).readEntry("LastCheck", QString()), Qt::ISODate);
}

bool UpdateChecker::announce(bool afterLogin) const
{
    const QString version = available();
    if (version.isEmpty()) {
        return false;
    }
    if (afterLogin) {
        return true;
    }
    KConfig config(stateFile(), KConfig::SimpleConfig);
    const KConfigGroup group = state(config);
    const QDateTime last = QDateTime::fromString(group.readEntry("AnnouncedAt", QString()), Qt::ISODate);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    return group.readEntry("Announced", QString()) != version || !last.isValid() || last > now || last.secsTo(now) >= remindEverySecs;
}

void UpdateChecker::markAnnounced()
{
    const QString version = available();
    KConfig config(stateFile(), KConfig::SimpleConfig);
    KConfigGroup group = state(config);
    group.writeEntry("Announced", version);
    group.writeEntry("AnnouncedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    config.sync();
}

void UpdateChecker::skip(const QString &version)
{
    KConfig config(stateFile(), KConfig::SimpleConfig);
    state(config).writeEntry("Skipped", UpdateCheck::cleanVersion(version));
    config.sync();
}

}
