/*
 * jignle-connection.cpp - Jingle Connection - minimal data transfer unit for an application
 * Copyright (C) 2021  Sergey Ilinykh
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
 *
 */

#include "jingle-connection.h"

namespace XMPP { namespace Jingle {

    bool Connection::hasPendingDatagrams() const { return false; }

    NetworkDatagram Connection::receiveDatagram(qint64 maxSize)
    {
        Q_UNUSED(maxSize)
        return NetworkDatagram();
    }

    size_t Connection::blockSize() const
    {
        return 0; // means "block" is not applicable for this kind of connection
    }

    int Connection::component() const { return 0; }
}}