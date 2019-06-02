/*
 * jignle-ibb.cpp - Jingle In-Band Bytestream transport
 * Copyright (C) 2019  Sergey Ilinykh
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

#include "jingle-ibb.h"

namespace XMPP {
namespace Jingle {
namespace IBB {

struct Transport::Private
{
    TransportManagerPad::Ptr pad;
};

Transport::Transport(const TransportManagerPad::Ptr &pad)
{

}

Transport::Transport(const TransportManagerPad::Ptr &pad, const QDomElement &transportEl)
{

}

Transport::~Transport()
{

}

TransportManagerPad::Ptr Transport::pad() const
{
    return d->pad;
}

void Transport::prepare()
{

}

void Transport::start()
{

}

bool Transport::update(const QDomElement &transportEl)
{

}

bool Transport::hasUpdates() const
{

}

OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate()
{

}

bool Transport::isValid() const
{
    return true;
}

Transport::Features Transport::features() const
{

}

QString Transport::sid() const
{

}

Connection::Ptr Transport::connection() const
{

}

size_t Transport::blockSize() const
{

}

QSharedPointer<XMPP::Jingle::Transport> Transport::createOutgoing(const TransportManagerPad::Ptr &pad)
{
    return QSharedPointer<XMPP::Jingle::Transport>();
}

QSharedPointer<XMPP::Jingle::Transport> Transport::createIncoming(const TransportManagerPad::Ptr &pad, const QDomElement &transportEl)
{
    return QSharedPointer<XMPP::Jingle::Transport>();
}

Pad::Pad(Manager *manager, Session *session)
{

}

QString Pad::ns() const
{
    return NS;
}

Session *Pad::session() const
{

}

TransportManager *Pad::manager() const
{

}

QString Pad::generateSid() const
{

}

void Pad::registerSid(const QString &sid)
{

}

} // namespace IBB
} // namespace Jingle
} // namespace XMPP
