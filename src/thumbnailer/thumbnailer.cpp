// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KIO/ThumbnailCreator>
#include <KPluginFactory>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QImage>

#include <algorithm>

namespace
{
// Graph usually answers within a second; Dolphin's preview job waits for this file meanwhile
constexpr int replyTimeoutMs = 10000;
}

// Previews of online-only files (Files On-Demand placeholders, name.cloud): graphsyncd fetches the
// thumbnail Microsoft renders for the real file. Dolphin caches the result like any thumbnail.
class PlaceholderThumbnailer : public KIO::ThumbnailCreator
{
    Q_OBJECT
public:
    PlaceholderThumbnailer(QObject *parent, const QVariantList &args)
        : KIO::ThumbnailCreator(parent, args)
    {
    }

    KIO::ThumbnailResult create(const KIO::ThumbnailRequest &request) override
    {
        if (!request.url().isLocalFile()) {
            return KIO::ThumbnailResult::fail();
        }
        const QSize target = request.targetSize();
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("io.github.nvaert1986.GraphSync"),
                                                           QStringLiteral("/GraphSync"),
                                                           QStringLiteral("io.github.nvaert1986.GraphSync"),
                                                           QStringLiteral("Thumbnail"));
        // A preview never starts the sync service; without it there is no sign-in to ask with
        call.setAutoStartService(false);
        call << request.url().toLocalFile() << std::max(target.width(), target.height());
        const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, replyTimeoutMs);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            return KIO::ThumbnailResult::fail();
        }
        QImage image = QImage::fromData(reply.arguments().constFirst().toByteArray());
        if (image.isNull()) {
            return KIO::ThumbnailResult::fail();
        }
        if (image.width() > target.width() || image.height() > target.height()) {
            image = image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        return KIO::ThumbnailResult::pass(image);
    }
};

K_PLUGIN_CLASS_WITH_JSON(PlaceholderThumbnailer, "thumbnailer.json")

#include "thumbnailer.moc"
