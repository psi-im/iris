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

#ifndef XMPP_IPUTIL_H
#define XMPP_IPUTIL_H

class QHostAddress;

namespace XMPP {

class IpUtil {
public:
    // NOTE: Keep scopes in priority wrt to connectivity speed
    enum AddressScope { Loopback, LinkLocal, SiteLocal, Private, UniqueLocalUnicast, Global };

    // TODO with Qt 5.11+ all the is*Addrress except Private have to be removed
    static inline bool isGlobalAddress(const QHostAddress &a)
    {
        auto s = addressScope(a);
        return s == Global || s == SiteLocal;
    }
    static inline bool isLoopbackAddress(const QHostAddress &a) { return addressScope(a) == Loopback; }
    static inline bool isLinkLocalAddress(const QHostAddress &a) { return addressScope(a) == LinkLocal; }
    static inline bool isSiteLocalAddress(const QHostAddress &a) { return addressScope(a) == SiteLocal; }
    static inline bool isPrivateAddress(const QHostAddress &a) { return addressScope(a) == Private; }
    static inline bool isUniqueLocalUnicastAddress(const QHostAddress &a)
    {
        return addressScope(a) == UniqueLocalUnicast;
    }

    static AddressScope addressScope(const QHostAddress &a);
};

} // namespace XMPP

#endif // XMPP_IPUTIL_H
