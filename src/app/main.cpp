// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "backend.h"

#include <KAboutData>
#include <KDBusService>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KWindowSystem>

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

using namespace GraphSync;

namespace
{
struct StartRequest {
    QString mode;
    QString argument;
};

// Shared by the first start and by later starts forwarded through KDBusService, so both
// open the same page for the same arguments
StartRequest parseRequest(QCommandLineParser &parser, const Backend &backend)
{
    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty() && positional.constFirst().startsWith(QLatin1String("odopen:"))) {
        return {QStringLiteral("odopen"), positional.constFirst()};
    }
    if (parser.isSet(QStringLiteral("share"))) {
        return {QStringLiteral("share"), QFileInfo(parser.value(QStringLiteral("share"))).absoluteFilePath()};
    }
    if (parser.isSet(QStringLiteral("versions"))) {
        return {QStringLiteral("versions"), QFileInfo(parser.value(QStringLiteral("versions"))).absoluteFilePath()};
    }
    if (parser.isSet(QStringLiteral("choose-folders"))) {
        return {QStringLiteral("chooseFolders"), parser.value(QStringLiteral("choose-folders"))};
    }
    if (parser.isSet(QStringLiteral("sign-in"))) {
        return {QStringLiteral("signIn"), {}};
    }
    if (parser.isSet(QStringLiteral("add"))) {
        return {QStringLiteral("add"), {}};
    }
    if (parser.isSet(QStringLiteral("setup")) || !backend.setupDone()) {
        return {QStringLiteral("setup"), {}};
    }
    // Like OneDrive's Settings window: what is synced, its state and the account
    return {QStringLiteral("settings"), {}};
}

void setupParser(QCommandLineParser &parser)
{
    parser.addOption({QStringLiteral("setup"), i18n("Run the setup wizard")});
    parser.addOption({QStringLiteral("add"), i18n("Add a library")});
    parser.addOption({QStringLiteral("sign-in"), i18n("Sign in again")});
    parser.addOption({QStringLiteral("choose-folders"), i18n("Choose which folders of a library to sync"), QStringLiteral("root")});
    parser.addOption({QStringLiteral("add-personal"), i18n("Start syncing your own OneDrive")});
    parser.addOption({QStringLiteral("copy-link"), i18n("Copy a link to a synced file or folder"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("view-online"), i18n("Open a synced file or folder in the browser"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("download"), i18n("Download the given online-only files without opening them")});
    parser.addOption({QStringLiteral("share"), i18n("Share a synced file or folder"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("versions"), i18n("Show the version history of a synced file"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("screenshot"), QStringLiteral("Save a screenshot of the first page and quit (testing)"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("wizard-step"), QStringLiteral("Open the setup wizard at this step (testing)"), QStringLiteral("step")});
    parser.addOption({QStringLiteral("settings-section"), QStringLiteral("Open the settings at this section (testing)"), QStringLiteral("index")});
    parser.addPositionalArgument(QStringLiteral("url"), i18n("odopen:// link from the SharePoint Sync button, a .url shortcut file or an online-only .cloud file"));
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("plasma-graphsync-linux");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

    KAboutData about(QStringLiteral("plasma-graphsync-linux"),
                     i18n("Graph Sync"),
                     QStringLiteral(GRAPHSYNC_VERSION),
                     i18n("Sync client for Microsoft OneDrive and SharePoint on KDE Plasma"),
                     KAboutLicense::GPL_V3,
                     i18n("Not affiliated with or endorsed by Microsoft. OneDrive and SharePoint are trademarks of Microsoft Corporation."));
    about.setOrganizationDomain("nvaert1986.github.io");
    about.setDesktopFileName(QStringLiteral("io.github.nvaert1986.graphsync"));
    KAboutData::setApplicationData(about);
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.nvaert1986.graphsync"), QIcon::fromTheme(QStringLiteral("folder-cloud"))));

    QCommandLineParser parser;
    about.setupCommandLine(&parser);
    setupParser(parser);
    parser.process(app);
    about.processCommandLine(&parser);

    Backend backend;
    if (parser.isSet(QStringLiteral("add-personal"))) {
        // Turned on from the settings: no window needed
        return backend.addPersonal().isEmpty() ? 1 : 0;
    }
    // .url files ("Add shortcut to My files" on a file): open the address, no window
    if (const QStringList positional = parser.positionalArguments();
        !positional.isEmpty() && positional.constFirst().endsWith(QLatin1String(".url"), Qt::CaseInsensitive)) {
        return Backend::openInternetShortcut(positional.constFirst());
    }
    // Open With > Download Now: only download the placeholders' files; no window
    if (parser.isSet(QStringLiteral("download"))) {
        return backend.downloadPlaceholders(parser.positionalArguments());
    }
    // .cloud placeholders (Files On-Demand): download, then open in the default program; no window
    if (const QStringList positional = parser.positionalArguments();
        !positional.isEmpty() && positional.constFirst().endsWith(QLatin1String(".cloud"), Qt::CaseInsensitive)) {
        return backend.openPlaceholder(positional.constFirst());
    }
    if (parser.isSet(QStringLiteral("view-online"))) {
        return backend.viewOnline(QFileInfo(parser.value(QStringLiteral("view-online"))).absoluteFilePath());
    }
    if (parser.isSet(QStringLiteral("copy-link"))) {
        // One click from Dolphin or the tray: no window, the result arrives as a notification
        return backend.copyLink(QFileInfo(parser.value(QStringLiteral("copy-link"))).absoluteFilePath());
    }

    // One instance, like OneDrive: a second start (menu, Sync button, tray) hands its
    // arguments to this one and exits. Screenshots (tests) always run on their own.
    std::unique_ptr<KDBusService> unique;
    if (!parser.isSet(QStringLiteral("screenshot"))) {
        unique = std::make_unique<KDBusService>(KDBusService::Unique);
    }

    const StartRequest start = parseRequest(parser, backend);

    QQmlApplicationEngine engine;
    KLocalization::setupLocalizedContext(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("startMode"), start.mode);
    engine.rootContext()->setContextProperty(QStringLiteral("startArgument"), start.argument);
    engine.rootContext()->setContextProperty(QStringLiteral("startSection"), parser.value(QStringLiteral("settings-section")).toInt());
    engine.rootContext()->setContextProperty(QStringLiteral("startStep"),
                                             parser.isSet(QStringLiteral("wizard-step")) ? parser.value(QStringLiteral("wizard-step")).toInt() : 0);
    engine.loadFromModule("io.github.nvaert1986.graphsync.app", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());

    if (unique) {
        QObject::connect(unique.get(), &KDBusService::activateRequested, &backend, [&backend, window](const QStringList &arguments, const QString &) {
            QCommandLineParser forwarded;
            setupParser(forwarded);
            forwarded.parse(arguments);
            const StartRequest request = parseRequest(forwarded, backend);
            Q_EMIT backend.activateRequested(request.mode, request.argument);
            // The Wayland activation token arrives with the request; without it KWin would
            // only flash the task bar entry
            KWindowSystem::updateStartupId(window);
            KWindowSystem::activateWindow(window);
        });
    }

    if (parser.isSet(QStringLiteral("screenshot"))) {
        const QString file = parser.value(QStringLiteral("screenshot"));
        QTimer::singleShot(1500, window, [window, file] {
            window->grabWindow().save(file);
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
