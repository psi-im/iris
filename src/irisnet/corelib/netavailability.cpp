/*
 * Copyright (C) 2008  Justin Karneges
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

#include "netavailability.h"

namespace XMPP {
class NetAvailability::Private : public QObject
{
    Q_OBJECT

public:
    NetAvailability *q;

    Private(NetAvailability *_q) :
        QObject(_q),
        q(_q)
    {
    }
};

NetAvailability::NetAvailability(QObject *parent) :
    QObject(parent)
{
    d = new Private(this);
}

NetAvailability::~NetAvailability()
{
    delete d;
}

bool NetAvailability::isAvailable() const
{
    // TODO
    return true;
}

} // namespace XMPP

#include "netavailability.moc"
