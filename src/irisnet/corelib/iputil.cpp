/*
 * Copyright (C) 2021  Psi IM team.
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

#include "iputil.h"

#include <QHostAddress>

namespace XMPP {

IpUtil::AddressScope IpUtil::addressScope(const QHostAddress &a)
{
    if (a.isLoopback())
        return Loopback;
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    if (a.isLinkLocal())
        return LinkLocal;
    if (a.isSiteLocal())
        return SiteLocal;
    if (a.isUniqueLocalUnicast())
        return UniqueLocalUnicast;
    // check for private
    if (a.protocol() == QAbstractSocket::IPv6Protocol) {
        if (a.toIPv6Address()[0] == 0xfd)
            return Private;
    } else if (a.protocol() == QAbstractSocket::IPv4Protocol) {
        quint32 v4 = a.toIPv4Address();
        quint8  a0 = quint8(v4 >> 24);
        quint8  a1 = quint8((v4 >> 16) & 0xff);
        if (a0 == 10 || (a0 == 172 && a1 >= 16 && a1 <= 31) || (a0 == 192 && a1 == 168))
            return Private;
    }
#else
    if (a.protocol() == QAbstractSocket::IPv6Protocol) {
        Q_IPV6ADDR addr6 = addr.toIPv6Address();
        quint16    hi    = addr6[0];
        hi <<= 8;
        hi += addr6[1];
        if ((hi & 0xffc0) == 0xfe80)
            return LinkLocal;
        if ((hi & 0xffc0) == 0xfec0)
            return SiteLocal;
        if ((hi & 0xff00) == 0xfd00)
            return Private;
        if ((hi & 0xfe00) == 0xfc00)
            return UniqueLocalUnicast;
    } else if (a.protocol() == QAbstractSocket::IPv4Protocol) {
        quint32 v4 = a.toIPv4Address();
        quint8  a0 = quint8(v4 >> 24);
        quint8  a1 = quint8((v4 >> 16) & 0xff);
        if (a0 == 169 && a1 == 254)
            return LinkLocal;
        else if (a0 == 10)
            return Private;
        else if (a0 == 172 && a1 >= 16 && a1 <= 31)
            return Private;
        else if (a0 == 192 && a1 == 168)
            return Private;
    }
#endif

    return Global;
}

} // namespace XMPP
