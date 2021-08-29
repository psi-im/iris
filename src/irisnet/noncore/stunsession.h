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

#ifndef XMPP_STUNSESSION_H
#define XMPP_STUNSESSION_H

#include <QObject>

#include "iceabstractstundisco.h"
#include "transportaddress.h"

namespace XMPP {

class StunSession : public QObject {
    Q_OBJECT
public:
    enum Type { StunServer, RelayServer, Peer };

    explicit StunSession(AbstractStunDisco::Service::Ptr service, QObject *parent = nullptr);
    explicit StunSession(const QHostAddress &peerAddr, std::uint16_t peerPort, QObject *parent = nullptr);

    void start();

signals:
};

} // namespace XMPP

#endif // XMPP_STUNSESSION_H
