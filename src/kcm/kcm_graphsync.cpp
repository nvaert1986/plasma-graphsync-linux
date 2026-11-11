// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "kcm_graphsync.h"
#include "backend.h"

#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(KcmGraphSync, "kcm_graphsync.json")

KcmGraphSync::KcmGraphSync(QObject *parent, const KPluginMetaData &data)
    : KQuickConfigModule(parent, data)
    , m_backend(new GraphSync::Backend(this))
{
    setButtons(NoAdditionalButton);
}

QObject *KcmGraphSync::backend() const
{
    return m_backend;
}

#include "kcm_graphsync.moc"
