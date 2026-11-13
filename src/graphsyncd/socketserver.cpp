// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "socketserver.h"
#include "daemon.h"
#include "ignorerules.h"
#include "localscanner.h"
#include "placeholder.h"
#include "graphsync_debug.h"
#include "rootcontroller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>

namespace GraphSync
{

namespace
{
constexpr int updateCoalesceMs = 500;
constexpr qint64 noSyncCacheMs = 3000;

bool anyUnder(const QSet<QString> &paths, const QString &rel)
{
    for (const QString &path : paths) {
        if (path == rel || path.startsWith(rel + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}
}

SocketServer::SocketServer(Daemon *daemon, QObject *parent)
    : QObject(parent)
    , m_daemon(daemon)
{
    m_updateTimer = new QTimer(this);
    m_updateTimer->setSingleShot(true);
    m_updateTimer->setInterval(updateCoalesceMs);
    connect(m_updateTimer, &QTimer::timeout, this, [this] {
        for (const QString &root : std::as_const(m_pendingUpdates)) {
            broadcast(QStringLiteral("UPDATE_VIEW:") + root);
        }
        m_pendingUpdates.clear();
    });
    connect(daemon, &Daemon::rootsChanged, this, &SocketServer::watchRoots);
    watchRoots();
}

SocketServer::~SocketServer()
{
    if (m_server) {
        m_server->close();
    }
}

QString SocketServer::socketPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/plasma-graphsync-linux/socket");
}

bool SocketServer::listen()
{
    const QString path = socketPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    // A stale socket from a crashed instance; graphsyncd is single-instance through its D-Bus name
    QLocalServer::removeServer(path);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(path)) {
        qCWarning(GRAPHSYNC) << "Cannot listen on" << path << m_server->errorString();
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, &SocketServer::onNewConnection);
    return true;
}

void SocketServer::watchRoots()
{
    QSet<QString> current;
    const QList<RootController *> roots = m_daemon->controllers();
    for (RootController *root : roots) {
        current.insert(root->localPath());
        // Connections are per controller; Qt::UniqueConnection does not work with lambdas
        disconnect(root, &RootController::snapshotChanged, this, nullptr);
        connect(root, &RootController::snapshotChanged, this, [this, path = root->localPath()] {
            scheduleUpdate(path);
        });
    }
    for (const QString &gone : m_registered - current) {
        broadcast(QStringLiteral("UNREGISTER_PATH:") + gone);
    }
    for (const QString &added : current - m_registered) {
        broadcast(QStringLiteral("REGISTER_PATH:") + added);
    }
    m_registered = current;
}

void SocketServer::scheduleUpdate(const QString &root)
{
    m_pendingUpdates.insert(root);
    if (!m_updateTimer->isActive()) {
        m_updateTimer->start();
    }
}

void SocketServer::onNewConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        m_clients.append(socket);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            onReadyRead(socket);
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            m_clients.removeAll(socket);
            socket->deleteLater();
        });
        for (const QString &root : std::as_const(m_registered)) {
            send(socket, QStringLiteral("REGISTER_PATH:") + root);
        }
    }
}

void SocketServer::onReadyRead(QLocalSocket *socket)
{
    while (socket->canReadLine()) {
        // Only the line end goes: spaces at either end belong to the file name
        QByteArray raw = socket->readLine();
        if (raw.endsWith('\n')) {
            raw.chop(1);
        }
        if (raw.endsWith('\r')) {
            raw.chop(1);
        }
        const QString line = QString::fromUtf8(raw);
        if (!line.isEmpty()) {
            handleLine(socket, line);
        }
    }
}

void SocketServer::handleLine(QLocalSocket *socket, const QString &line)
{
    const int colon = line.indexOf(QLatin1Char(':'));
    const QString command = colon < 0 ? line : line.left(colon);
    const QString argument = colon < 0 ? QString() : line.mid(colon + 1);

    if (command == QLatin1String("RETRIEVE_FILE_STATUS") || command == QLatin1String("RETRIEVE_FOLDER_STATUS")) {
        const QString path = QDir::cleanPath(argument);
        send(socket, QStringLiteral("STATUS:%1:%2").arg(statusFor(path), path));
    } else if (command == QLatin1String("VERSION")) {
        send(socket, QStringLiteral("VERSION:%1:1.0:%2").arg(QCoreApplication::applicationVersion()).arg(QCoreApplication::applicationPid()));
    }
}

const RootController *SocketServer::ownerOf(const QString &path) const
{
    // The longest match: a shortcut's folder lies inside the OneDrive that holds it
    QString rel;
    return m_daemon->controllerFor(path, &rel);
}

bool SocketServer::hasNoSyncMarker(const QString &folder) const
{
    // Dolphin asks for every item of a folder at once: each folder above them is looked at once,
    // and again after a few seconds (a .nosync file may come or go)
    if (!m_noSyncAge.isValid() || m_noSyncAge.hasExpired(noSyncCacheMs)) {
        m_noSync.clear();
        m_noSyncAge.start();
    }
    auto it = m_noSync.constFind(folder);
    if (it == m_noSync.constEnd()) {
        it = m_noSync.insert(folder, QFileInfo::exists(folder + QLatin1Char('/') + IgnoreRules::noSyncMarker));
    }
    return *it;
}

QString SocketServer::statusFor(const QString &path) const
{
    const RootController *owner = ownerOf(path);
    if (!owner) {
        return QStringLiteral("NOP");
    }
    const auto snapshot = owner->snapshot();
    const QString status = syncStatusFor(path, owner, snapshot);
    if (status == QLatin1String("NOP") || status == QLatin1String("IGNORE") || status.endsWith(QLatin1String("+ROOT"))) {
        return status;
    }
    // Shared with others: the item itself or a folder above it has sharing of its own
    if (!snapshot || snapshot->shared.isEmpty()) {
        return status;
    }
    QString rel = path.mid(owner->localPath().size() + 1);
    if (Placeholder::hasSuffix(rel) && snapshot->dehydrated.contains(Placeholder::realPathOf(rel))) {
        rel = Placeholder::realPathOf(rel);
    }
    for (QString part = rel; !part.isEmpty(); part = part.section(QLatin1Char('/'), 0, -2)) {
        if (snapshot->shared.contains(part)) {
            return status + QStringLiteral("+SHARED");
        }
    }
    return status;
}

QString SocketServer::syncStatusFor(const QString &path, const RootController *owner, const std::shared_ptr<const SyncEngine::StatusSnapshot> &snapshot) const
{
    if (path == owner->localPath()) {
        // A copy of the whole engine status: only for the root itself
        const QString state = owner->status().value(QStringLiteral("state")).toString();
        QString status = QStringLiteral("OK");
        if (state == QLatin1String("error") || state == QLatin1String("halted") || (snapshot && !snapshot->errors.isEmpty())) {
            status = QStringLiteral("ERROR");
        } else if (state == QLatin1String("syncing") || (snapshot && !snapshot->busy.isEmpty())) {
            status = QStringLiteral("SYNC");
        }
        return status + QStringLiteral("+ROOT");
    }
    if (!snapshot) {
        return QStringLiteral("SYNC");
    }

    const QString rel = path.mid(owner->localPath().size() + 1);
    // Files On-Demand: the placeholder of an online-only file
    if (Placeholder::hasSuffix(rel) && snapshot->dehydrated.contains(Placeholder::realPathOf(rel))) {
        return snapshot->busy.contains(Placeholder::realPathOf(rel)) ? QStringLiteral("SYNC") : QStringLiteral("ONLINE");
    }
    const QStringList parts = rel.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (IgnoreRules::isIgnored(part)) {
            return QStringLiteral("IGNORE");
        }
    }
    // The user's own exclusions, and folders holding a .nosync file
    if (snapshot->userRules.matchesPath(rel)) {
        return QStringLiteral("IGNORE");
    }
    QString folder = owner->localPath();
    for (const QString &part : parts) {
        folder += QLatin1Char('/') + part;
        if (hasNoSyncMarker(folder)) {
            return QStringLiteral("IGNORE");
        }
    }
    for (const QString &excluded : snapshot->excluded) {
        if (rel == excluded || rel.startsWith(excluded + QLatin1Char('/'))) {
            return QStringLiteral("IGNORE");
        }
    }
    if (anyUnder(snapshot->errors, rel)) {
        return QStringLiteral("ERROR");
    }
    if (anyUnder(snapshot->busy, rel)) {
        return QStringLiteral("SYNC");
    }

    const auto known = snapshot->known.constFind(rel);
    if (known == snapshot->known.constEnd()) {
        // Not synced yet: new here and waiting for the next cycle
        return QStringLiteral("NEW");
    }
    if (known->isFolder) {
        return QStringLiteral("OK");
    }
    const auto local = LocalScanner::stat(path);
    if (!local) {
        return QStringLiteral("NOP");
    }
    // Changed since the last sync: will be uploaded with the next cycle
    return (local->size == known->size && local->mtimeNs == known->mtimeNs) ? QStringLiteral("OK") : QStringLiteral("SYNC");
}

void SocketServer::send(QLocalSocket *socket, const QString &line)
{
    socket->write(line.toUtf8() + '\n');
}

void SocketServer::broadcast(const QString &line)
{
    for (QLocalSocket *socket : std::as_const(m_clients)) {
        send(socket, line);
    }
}

}
