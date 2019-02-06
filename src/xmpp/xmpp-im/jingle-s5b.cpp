/*
 * jignle-s5b.cpp - Jingle SOCKS5 transport
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "jingle-s5b.h"
#include "xmpp/jid/jid.h"

namespace XMPP {
namespace Jingle {
namespace S5B {

const QString NS(QStringLiteral("urn:xmpp:jingle:transports:s5b:1"));

class Candidate::Private : public QSharedData {
public:
    QString cid;
    QString host;
    Jid jid;
    quint16 port;
    quint16 priority;
    Candidate::Type type;
};

Candidate::Candidate(const QDomElement &el) :
    d(new Private)
{
    bool ok;
    d->host = el.attribute(QStringLiteral("host"));
    d->jid = Jid(el.attribute(QStringLiteral("jid")));
    auto port = el.attribute(QStringLiteral("port"));
    if (!port.isEmpty()) {
        d->port = port.toUShort(&ok);
        if (!ok) {
            return; // make the whole candidate invalid
        }
    }
    auto priority = el.attribute(QStringLiteral("priority"));
    if (!priority.isEmpty()) {
        d->priority = priority.toUShort(&ok);
        if (!ok) {
            return; // make the whole candidate invalid
        }
    }
    d->cid = el.attribute(QStringLiteral("cid"));
}

Candidate::Candidate(const Candidate &other) :
    d(other.d)
{

}

Candidate::~Candidate()
{

}

class S5BTransport::Private {
public:
    QList<Candidate> candidates;
    QString dstaddr;
    QString sid;
    S5BTransport::Mode mode;
    Transport::Direction direction;
};

S5BTransport::S5BTransport(const QDomElement &el) :
    d(new Private)
{
    d->sid = el.attribute(QStringLiteral("sid"));
    d->direction = Transport::Incoming;
    // TODO remaining
    if (d->sid.isEmpty()) { // is invalid
        d.reset(); //  make invalid
    }
}

S5BTransport::~S5BTransport()
{

}

void S5BTransport::start()
{

}

bool S5BTransport::update(const QDomElement &el)
{
    Q_UNUSED(el)
    return false; // TODO
}

QDomElement S5BTransport::takeUpdate(QDomDocument *doc)
{
    Q_UNUSED(doc)
    return QDomElement(); // TODO
}

bool S5BTransport::isValid() const
{
    return d != nullptr;
}

QSharedPointer<Transport> S5BTransport::createOutgoing()
{
    auto d = new Private;
    d->direction = Transport::Outgoing;
    d->sid = QString("s5b_%1").arg(qrand() & 0xffff, 4, 16, QChar('0')); // FIXME check for collisions

    auto t = new S5BTransport;
    t->d.reset(d);
    return QSharedPointer<Transport>(t);
}

QSharedPointer<Transport> Manager::sessionInitiate()
{
    return S5BTransport::createOutgoing();
}

QSharedPointer<Transport> Manager::sessionInitiate(const QDomElement &transportEl)
{
    QSharedPointer<Transport> t(new S5BTransport(transportEl));
    if (!t->isValid()) {
        t.reset();
    }
    return t;
}


} // namespace S5B
} // namespace Jingle
} // namespace XMPP
