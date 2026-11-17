// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QUrl>

#include <optional>

class QNetworkAccessManager;

namespace GraphSync
{

// New versions: the packages on GitHub come from no repository, so no package manager tells their
// users about a new release. Once a day the newest release is asked from GitHub; nothing but the
// request itself is sent.
namespace UpdateCheck
{
// Built with the check (CMake option GRAPHSYNC_UPDATE_CHECK); distributions that ship updates
// themselves leave it out
bool compiledIn();

struct Release {
    QString version;
    // The release page with the packages and the notes
    QUrl page;
};

// The release in GitHub's answer to releases/latest; nullopt for a draft, a pre-release or an
// answer that is not understood
std::optional<Release> parseLatest(const QByteArray &json);
// A plain version (1.2.3, also with a leading v) or empty when it is none
QString cleanVersion(const QString &tag);
// candidate is a later version than current
bool isNewer(const QString &candidate, const QString &current);
// The version running here
QString currentVersion();
}

// Asks GitHub and keeps what it said in the state folder, so a restart does not ask again
class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // Asks GitHub when the last answer is a day old (force: now); checked() follows. Without asking,
    // it only looks at the last answer again.
    void check(bool force = false);
    // The newer version from the last answer, unless skipped; empty when there is none
    QString available() const;
    QUrl page() const;
    // The newest release in GitHub's last answer, also when it is not newer than this version
    QString latest() const;
    // When GitHub last answered (invalid: never)
    QDateTime lastCheck() const;
    // Waiting for GitHub
    bool busy() const
    {
        return m_busy;
    }
    // A newer version is told about at every login (afterLogin) and once a day, until skipped:
    // announce() says whether it is time, markAnnounced() remembers when it was told
    bool announce(bool afterLogin) const;
    void markAnnounced();
    // "Skip This Version": no notification and no hint in the tray for it
    void skip(const QString &version);

Q_SIGNALS:
    // ok false: GitHub could not be asked (error says why); the next check tries again
    void checked(bool ok, const QString &error);

private:
    QNetworkAccessManager *m_network = nullptr;
    bool m_busy = false;
};

}
