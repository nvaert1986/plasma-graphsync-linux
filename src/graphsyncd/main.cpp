// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "daemon.h"
#include "dbusnames.h"
#include "dbusservice.h"
#include "graphsync_debug.h"
#include "migration.h"
#include "socketserver.h"

#include <KLocalizedString>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QGuiApplication>
#include <QSocketNotifier>

#include <csignal>
#include <cstdlib>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

using namespace GraphSync;

namespace
{
int signalPipe[2] = {-1, -1};

void handleSignal(int)
{
    const char byte = 1;
    [[maybe_unused]] const auto written = ::write(signalPipe[1], &byte, 1);
}
}

int main(int argc, char **argv)
{
    // Notifications need a GUI application; without a display (ssh, tests) run headless
    const bool haveDisplay = std::getenv("WAYLAND_DISPLAY") || std::getenv("DISPLAY");
    std::unique_ptr<QCoreApplication> app;
    if (haveDisplay) {
        QGuiApplication::setQuitOnLastWindowClosed(false);
        // Qt registers the process with the desktop portal at start, by its desktop file: the app's
        // (without it: "Failed to register with host portal ... App info not found")
        QGuiApplication::setDesktopFileName(QStringLiteral("io.github.nvaert1986.graphsync"));
        app = std::make_unique<QGuiApplication>(argc, argv);
    } else {
        app = std::make_unique<QCoreApplication>(argc, argv);
    }
    QCoreApplication::setApplicationName(QStringLiteral("plasma-graphsync-linux"));
    QCoreApplication::setApplicationVersion(QStringLiteral(GRAPHSYNC_VERSION));
    KLocalizedString::setApplicationDomain("plasma-graphsync-linux");
    // Accounts are shared by the suite; the app registration per account is this app's own
    GraphCore::Accounts::setApplication(QStringLiteral("plasma-graphsync-linux"));

    // SIGTERM/SIGINT: quit the event loop so every root finishes its cycle and saves state
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, signalPipe) == 0) {
        auto *notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, app.get());
        QObject::connect(notifier, &QSocketNotifier::activated, app.get(), [] {
            char byte;
            [[maybe_unused]] const auto n = ::read(signalPipe[0], &byte, 1);
            qCInfo(GRAPHSYNC) << "Stopping";
            QCoreApplication::quit();
        });
        std::signal(SIGTERM, handleSignal);
        std::signal(SIGINT, handleSignal);
    }

    // An install from before 0.7.0 (plasma-onedrive-linux) is taken over first; this also stops
    // its daemon, so the two never sync the same folders
    // Short lock wait: when another process is importing or the old daemon is still stopping, exit
    // and let systemd start us again (Restart=on-failure) instead of running without the settings.
    // A permanent failure is not retried in a loop: the app shows it, sync runs without the import
    QString migration;
    switch (Migration::importLegacyInstall(&migration, 5000)) {
    case Migration::Result::Busy:
        qCWarning(GRAPHSYNC).noquote() << "Taking over plasma-onedrive-linux has to wait, retrying later:" << migration;
        return 1;
    case Migration::Result::Failed:
        qCWarning(GRAPHSYNC).noquote() << "Taking over plasma-onedrive-linux failed:" << migration;
        break;
    case Migration::Result::NotNeeded:
    case Migration::Result::Imported:
        break;
    }
    Migration::adoptSharedAccounts();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerService(DBusNames::service())) {
        qCCritical(GRAPHSYNC) << "graphsyncd is already running (or the session bus is unavailable)";
        return 1;
    }

    Daemon daemon;
    DBusService service(&daemon);
    bus.registerObject(DBusNames::path(), &service, QDBusConnection::ExportScriptableContents);
    SocketServer socketServer(&daemon);
    socketServer.listen();

    return QCoreApplication::exec();
}
