/*
 * proxyprotocol.cpp - PROXY protocol v1 header generation
 * Copyright (C) 2026  The Psi+ Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "proxyprotocol.h"

namespace XMPP {
bool createProxyProtocolV1Header(const QHostAddress &sourceAddress, quint16 sourcePort,
                                 const QHostAddress &destinationAddress, quint16 destinationPort, QByteArray &header,
                                 QString *error)
{
    header.clear();
    if (sourceAddress.isNull() || destinationAddress.isNull()) {
        if (error)
            *error = QStringLiteral("Local and peer IP addresses are required for a PROXY protocol header.");
        return false;
    }

    const auto sourceProtocol      = sourceAddress.protocol();
    const auto destinationProtocol = destinationAddress.protocol();
    if (sourceProtocol != destinationProtocol
        || (sourceProtocol != QAbstractSocket::IPv4Protocol && sourceProtocol != QAbstractSocket::IPv6Protocol)) {
        if (error)
            *error = QStringLiteral("The local and peer addresses must use the same IPv4 or IPv6 protocol.");
        return false;
    }

    QHostAddress source = sourceAddress;
    QHostAddress destination = destinationAddress;
    source.setScopeId(QString());
    destination.setScopeId(QString());

    const QByteArray family = sourceProtocol == QAbstractSocket::IPv4Protocol ? QByteArrayLiteral("TCP4")
                                                                              : QByteArrayLiteral("TCP6");
    header = QByteArrayLiteral("PROXY ") + family + ' ' + source.toString().toLatin1() + ' '
        + destination.toString().toLatin1() + ' ' + QByteArray::number(sourcePort) + ' '
        + QByteArray::number(destinationPort) + "\r\n";
    if (error)
        error->clear();
    return true;
}
}
