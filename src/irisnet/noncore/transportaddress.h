/*
 * Copyright (C) 2021  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef TRANSPORTADDRESS_H
#define TRANSPORTADDRESS_H

#include <QHostAddress>

namespace XMPP {

class TransportAddress {
public:
    QHostAddress addr;
    quint16      port = 0;

    TransportAddress() = default;
    TransportAddress(const QHostAddress &_addr, quint16 _port) : addr(_addr), port(_port) { }

    bool isValid() const { return !addr.isNull(); }
    bool operator==(const TransportAddress &other) const { return addr == other.addr && port == other.port; }

    inline bool operator!=(const TransportAddress &other) const { return !operator==(other); }
    inline      operator QString() const
    {
        return QString(QLatin1String("%1:%2")).arg(addr.toString(), QString::number(port));
    }
};

inline uint qHash(const TransportAddress &key, uint seed = 0)
{
    return ::qHash(key.addr, seed) ^ ::qHash(key.port, seed);
}

}

#endif // TRANSPORTADDRESS_H
