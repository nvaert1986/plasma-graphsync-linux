// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KQuickConfigModule>

namespace GraphSync
{
class Backend;
}

// "plasma-graphsync-linux" page in System Settings: the app's settings view (SettingsView.qml)
// with the same Backend. Everything is applied immediately, so the page has no Apply button.
class KcmGraphSync : public KQuickConfigModule
{
    Q_OBJECT
    Q_PROPERTY(QObject *backend READ backend CONSTANT)
public:
    KcmGraphSync(QObject *parent, const KPluginMetaData &data);

    QObject *backend() const;

private:
    GraphSync::Backend *m_backend;
};
