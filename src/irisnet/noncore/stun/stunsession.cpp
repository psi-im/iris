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

#include "stunsession.h"

namespace XMPP {

StunSession::StunSession(AbstractStunDisco::Service::Ptr service, QObject *parent) : QObject(parent) { }

StunSession::StunSession(const QHostAddress &peerAddr, uint16_t peerPort, QObject *parent) { }

} // namespace XMPP
