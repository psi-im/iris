/*
 * proxyprotocol.h - PROXY protocol v1 header generation
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PROXYPROTOCOL_H
#define PROXYPROTOCOL_H

#include <QByteArray>
#include <QHostAddress>
#include <QString>

namespace XMPP {
bool createProxyProtocolV1Header(const QHostAddress &sourceAddress, quint16 sourcePort,
                                 const QHostAddress &destinationAddress, quint16 destinationPort, QByteArray &header,
                                 QString *error = nullptr);
}

#endif // PROXYPROTOCOL_H
