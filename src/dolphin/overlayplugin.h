// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KOverlayIconPlugin>

#include <QHash>
#include <QSet>
#include <QString>

class QLocalSocket;
class QTimer;

// Sync state emblems in Dolphin (and file dialogs), from graphsyncd's local socket.
// getOverlays() runs on the GUI thread, so it answers from a cache and asks the
// daemon in the background; the answer arrives as overlaysChanged().
class OverlayPlugin : public KOverlayIconPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "io.github.nvaert1986.graphsync.overlayplugin" FILE "overlayplugin.json")
public:
    explicit OverlayPlugin(QObject *parent = nullptr);

    QStringList getOverlays(const QUrl &url) override;

private:
    void connectToDaemon();
    void onDisconnected();
    void onReadyRead();
    void handleLine(const QString &line);
    void request(const QString &path);
    bool isUnderRoot(const QString &path) const;
    void setStatus(const QString &path, const QString &status);
    static QStringList overlaysFor(const QString &status);

    QLocalSocket *m_socket = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    QSet<QString> m_roots;
    // Paths Dolphin asked about -> last STATUS (empty while unanswered)
    QHash<QString, QString> m_status;
    // When each path was last asked for (a counter), to forget the oldest
    QHash<QString, quint64> m_lastAsked;
    quint64 m_askCounter = 0;
    void forgetOldest();
    // Asked before the daemon told us its roots; answered once its REGISTER_PATH arrives, dropped
    // a few seconds after the first one (m_earlyTimer)
    QSet<QString> m_early;
    QTimer *m_earlyTimer = nullptr;
    bool m_rootsKnown = false;
};
