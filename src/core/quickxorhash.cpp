// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quickxorhash.h"

#include <QFile>

namespace GraphSync
{

namespace
{
constexpr int widthBits = 160;
constexpr int shift = 11;
constexpr int widthBytes = widthBits / 8;
}

void QuickXorHash::addData(const char *data, qsizetype size)
{
    auto lane = static_cast<size_t>(m_length % widthBits);
    for (qsizetype i = 0; i < size; ++i) {
        m_lanes[lane] ^= static_cast<uint8_t>(data[i]);
        if (++lane == widthBits) {
            lane = 0;
        }
    }
    m_length += static_cast<uint64_t>(size);
}

QString QuickXorHash::base64Result() const
{
    std::array<uint8_t, widthBytes> out{};
    for (int r = 0; r < widthBits; ++r) {
        const uint8_t b = m_lanes[static_cast<size_t>(r)];
        if (!b) {
            continue;
        }
        const int bit = (r * shift) % widthBits;
        const int byteIndex = bit / 8;
        const int bitInByte = bit % 8;
        out[static_cast<size_t>(byteIndex)] ^= static_cast<uint8_t>(b << bitInByte);
        if (bitInByte) {
            // Bits past position 159 wrap around to bit 0 (160 is byte aligned)
            out[static_cast<size_t>((byteIndex + 1) % widthBytes)] ^= static_cast<uint8_t>(b >> (8 - bitInByte));
        }
    }
    uint64_t length = m_length;
    for (int i = 0; i < 8; ++i) {
        out[static_cast<size_t>(widthBytes - 8 + i)] ^= static_cast<uint8_t>(length & 0xff);
        length >>= 8;
    }
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(out.data()), widthBytes).toBase64());
}

QString QuickXorHash::hashFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QuickXorHash hash;
    QByteArray buffer(1 << 20, Qt::Uninitialized);
    while (true) {
        const qint64 n = file.read(buffer.data(), buffer.size());
        if (n < 0) {
            return {};
        }
        if (n == 0) {
            break;
        }
        hash.addData(buffer.constData(), n);
    }
    return hash.base64Result();
}

}
