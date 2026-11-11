// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KAbstractFileItemActionPlugin>

class KFileItemListProperties;

// Dolphin context menu for files and folders inside a sync root. Reads the roots from
// graphsyncrc directly, so the menu works even while the daemon is not running.
class ActionPlugin : public KAbstractFileItemActionPlugin
{
    Q_OBJECT
public:
    ActionPlugin(QObject *parent, const QVariantList &args);

    QList<QAction *> actions(const KFileItemListProperties &fileItemInfos, QWidget *parentWidget) override;
};
