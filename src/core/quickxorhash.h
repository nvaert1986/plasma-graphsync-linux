// SPDX-FileCopyrightText: 2026 plasma-graphsync-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <cstdint>

namespace GraphSync
{

// Microsoft's QuickXorHash, the only content hash OneDrive for Business and SharePoint report:
// a 160-bit XOR of every byte placed at bit offset (index * 11) mod 160, with the total length
// XORed into the last 8 bytes. Bytes 160 positions apart share an offset, so input is folded
// into 160 byte lanes and only placed when the result is requested.
class QuickXorHash
{
public:
    void addData(const char *data, qsizetype size);
    void addData(const QByteArray &data)
    {
        addData(data.constData(), data.size());
    }

    // Base64, as Graph returns it in file.hashes.quickXorHash
    QString base64Result() const;

    // Empty string when the file cannot be read.
    static QString hashFile(const QString &path);

private:
    std::array<uint8_t, 160> m_lanes{};
    uint64_t m_length = 0;
};

}
