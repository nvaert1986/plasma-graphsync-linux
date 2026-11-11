// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "overlayplugin.h"

#include <QDir>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace
{
constexpr int reconnectMs = 5000;
constexpr int maxEarlyPaths = 2000;

const QString emblemSynced = QStringLiteral("emblem-success");
const QString emblemSyncing = QStringLiteral("io.github.nvaert1986.graphsync-emblem-syncing");
const QString emblemError = QStringLiteral("emblem-error");
const QString emblemCloud = QStringLiteral("io.github.nvaert1986.graphsync-emblem-cloud");

bool isUnder(const QString &path, const QString &root)
{
    return path == root || path.startsWith(root + QLatin1Char('/'));
}
}

OverlayPlugin::OverlayPlugin(QObject *parent)
    : KOverlayIconPlugin(parent)
{
    m_socket = new QLocalSocket(this);
    connect(m_socket, &QLocalSocket::readyRead, this, &OverlayPlugin::onReadyRead);
    connect(m_socket, &QLocalSocket::disconnected, this, &OverlayPlugin::onDisconnected);
    connect(m_socket, &QLocalSocket::errorOccurred, this, [this] {
        if (m_socket->state() == QLocalSocket::UnconnectedState) {
            m_reconnectTimer->start();
        }
    });

    // The daemon may start after Dolphin, or restart; keep trying quietly
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    m_reconnectTimer->setInterval(reconnectMs);
    connect(m_reconnectTimer, &QTimer::timeout, this, &OverlayPlugin::connectToDaemon);

    connectToDaemon();
}

void OverlayPlugin::connectToDaemon()
{
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
        return;
    }
    m_socket->connectToServer(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/plasma-graphsync-linux/socket"));
}

void OverlayPlugin::onDisconnected()
{
    // Stale emblems are worse than none
    m_roots.clear();
    m_rootsKnown = false;
    for (auto it = m_status.begin(); it != m_status.end(); ++it) {
        if (!it->isEmpty()) {
            it->clear();
            Q_EMIT overlaysChanged(QUrl::fromLocalFile(it.key()), {});
        }
    }
    m_reconnectTimer->start();
}

QStringList OverlayPlugin::getOverlays(const QUrl &url)
{
    if (!url.isLocalFile()) {
        return {};
    }
    const QString path = QDir::cleanPath(url.toLocalFile());
    if (!isUnderRoot(path)) {
        if (!m_rootsKnown && m_early.size() < maxEarlyPaths) {
            m_early.insert(path);
        }
        return {};
    }
    // Always ask again: Dolphin calls this when it (re)loads an item, so the cache may be old
    request(path);
    return overlaysFor(m_status.value(path));
}

bool OverlayPlugin::isUnderRoot(const QString &path) const
{
    for (const QString &root : m_roots) {
        if (isUnder(path, root)) {
            return true;
        }
    }
    return false;
}

void OverlayPlugin::request(const QString &path)
{
    if (!m_status.contains(path)) {
        m_status.insert(path, QString());
    }
    if (m_socket->state() == QLocalSocket::ConnectedState) {
        m_socket->write("RETRIEVE_FILE_STATUS:" + path.toUtf8() + '\n');
    }
}

void OverlayPlugin::onReadyRead()
{
    while (m_socket->canReadLine()) {
        const QString line = QString::fromUtf8(m_socket->readLine()).chopped(1);
        if (!line.isEmpty()) {
            handleLine(line);
        }
    }
    if (m_rootsKnown) {
        m_early.clear();
    }
}

void OverlayPlugin::handleLine(const QString &line)
{
    const int colon = line.indexOf(QLatin1Char(':'));
    if (colon < 0) {
        return;
    }
    const QStringView command = QStringView(line).left(colon);
    const QString argument = line.mid(colon + 1);

    if (command == QLatin1String("STATUS")) {
        // STATUS:<status>:<path>; the path may contain colons itself
        const int second = argument.indexOf(QLatin1Char(':'));
        if (second > 0) {
            setStatus(argument.mid(second + 1), argument.left(second));
        }
    } else if (command == QLatin1String("REGISTER_PATH")) {
        m_roots.insert(argument);
        m_rootsKnown = true;
        // Dolphin may have asked before we knew this root (plugin just loaded, daemon restarted)
        QStringList paths = m_status.keys();
        paths += QStringList(m_early.cbegin(), m_early.cend());
        for (const QString &path : std::as_const(paths)) {
            if (isUnder(path, argument)) {
                request(path);
            }
        }
    } else if (command == QLatin1String("UNREGISTER_PATH")) {
        m_roots.remove(argument);
        for (auto it = m_status.begin(); it != m_status.end();) {
            if (isUnder(it.key(), argument)) {
                if (!it->isEmpty()) {
                    Q_EMIT overlaysChanged(QUrl::fromLocalFile(it.key()), {});
                }
                it = m_status.erase(it);
            } else {
                ++it;
            }
        }
    } else if (command == QLatin1String("UPDATE_VIEW")) {
        const QStringList paths = m_status.keys();
        for (const QString &path : paths) {
            if (isUnder(path, argument)) {
                request(path);
            }
        }
    }
}

void OverlayPlugin::setStatus(const QString &path, const QString &status)
{
    QString &cached = m_status[path];
    if (cached == status) {
        return;
    }
    const bool emblemsChanged = overlaysFor(cached) != overlaysFor(status);
    cached = status;
    if (emblemsChanged) {
        Q_EMIT overlaysChanged(QUrl::fromLocalFile(path), overlaysFor(status));
    }
}

QStringList OverlayPlugin::overlaysFor(const QString &status)
{
    // First overlay goes bottom-right, the second bottom-left
    QStringList overlays;
    if (status.startsWith(QLatin1String("OK"))) {
        overlays << emblemSynced;
    } else if (status.startsWith(QLatin1String("SYNC")) || status.startsWith(QLatin1String("NEW"))) {
        overlays << emblemSyncing;
    } else if (status.startsWith(QLatin1String("ERROR"))) {
        overlays << emblemError;
    } else if (status.startsWith(QLatin1String("ONLINE"))) {
        // Files On-Demand: only online, opening it downloads it
        overlays << emblemCloud;
    }
    if (status.endsWith(QLatin1String("+ROOT"))) {
        overlays << emblemCloud;
    }
    return overlays;
}

#include "moc_overlayplugin.cpp"
