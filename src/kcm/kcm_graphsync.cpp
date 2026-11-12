// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "kcm_graphsync.h"
#include "backend.h"

#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(KcmGraphSync, "kcm_graphsync.json")

namespace
{
GraphSync::Backend *makeBackend(QObject *parent)
{
    // Accounts are shared by the suite; the app registration per account is this app's own
    GraphCore::Accounts::setApplication(QStringLiteral("plasma-graphsync-linux"));
    return new GraphSync::Backend(parent);
}
}

KcmGraphSync::KcmGraphSync(QObject *parent, const KPluginMetaData &data)
    : KQuickConfigModule(parent, data)
    , m_backend(makeBackend(this))
{
    setButtons(NoAdditionalButton);
}

QObject *KcmGraphSync::backend() const
{
    return m_backend;
}

#include "kcm_graphsync.moc"
