/*
 * Copyright (C) 2021  Psi IM team
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

#include "localaddress.h"
#include "iputil.h"

namespace XMPP::ICE {

// -1 = a is higher priority, 1 = b is higher priority, 0 = equal
static int comparePriority(const ICE::LocalAddress &a, const ICE::LocalAddress &b)
{
    // prefer closer scope
    auto a_scope = IpUtil::addressScope(a.addr);
    auto b_scope = IpUtil::addressScope(b.addr);
    if (a_scope < b_scope)
        return -1;
    else if (a_scope > b_scope)
        return 1;

    // prefer ipv6
    if (a.addr.protocol() == QAbstractSocket::IPv6Protocol && b.addr.protocol() != QAbstractSocket::IPv6Protocol)
        return -1;
    else if (b.addr.protocol() == QAbstractSocket::IPv6Protocol && a.addr.protocol() != QAbstractSocket::IPv6Protocol)
        return 1;

    return 0;
}

QList<ICE::LocalAddress> LocalAddress::sort(const QList<ICE::LocalAddress> &in)
{
    QList<ICE::LocalAddress> out;

    for (const auto &a : in) {
        int at;
        for (at = 0; at < out.count(); ++at) {
            if (comparePriority(a, out[at]) < 0)
                break;
        }

        out.insert(at, a);
    }

    return out;
}

} // namespace XMPP::ICE
