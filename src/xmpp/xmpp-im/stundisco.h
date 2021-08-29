/*
 * stundisco.h - STUN/TURN service discoverer
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

#ifndef XMPP_STUNDISCO_H
#define XMPP_STUNDISCO_H

#include "iceabstractstundisco.h"
#include "xmpp_client.h"

#include <memory>

namespace XMPP {

class Client;

class StunDiscoManager : public QObject {
    Q_OBJECT
public:
    enum UseFlag { UseBind = 0x1, UseRelayUdp = 0x2, UseRelayTcp = 0x4 };
    Q_DECLARE_FLAGS(UseFlags, UseFlag)

    static constexpr UseFlags RelayUseFlags  = UseFlags { UseRelayUdp | UseRelayTcp };
    static constexpr UseFlags DirectUseFlags = UseBind;

    StunDiscoManager(Client *client);
    ~StunDiscoManager();

    AbstractStunDisco *createMonitor(UseFlags useFlags = UseFlags { UseBind | UseRelayUdp | UseRelayTcp });
    Client *           client() const;

    void setStunBindService(const QString &host, int port);
    void setStunRelayUdpService(const QString &host, int port, const QString &user, const QString &pass);
    void setStunRelayTcpService(const QString &host, int port, const QString &user, const QString &pass);

private:
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace XMPP

Q_DECLARE_OPERATORS_FOR_FLAGS(XMPP::StunDiscoManager::UseFlags)

#endif // XMPP_STUNDISCO_H
