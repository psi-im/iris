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

#ifndef ICE_LOCALADDRESS_H
#define ICE_LOCALADDRESS_H

#include <QHostAddress>
#include <QNetworkInterface>

namespace XMPP::ICE {

class LocalAddress {
public:
    QHostAddress                     addr;
    int                              network = 0; // 0 = unknown. see QNetworkInterface::index doc
    QNetworkInterface::InterfaceType type    = QNetworkInterface::Unknown;

    static QList<LocalAddress> sort(const QList<LocalAddress> &in);
};

} // namespace XMPP::ICE

#endif // ICE_LOCALADDRESS_H
