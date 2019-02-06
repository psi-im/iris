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

class Transport::Private {
public:
    Manager *manager = nullptr;
    QList<Candidate> candidates;
    QString dstaddr;
    QString sid;
    Transport::Mode mode = Transport::Tcp;
    Transport::Direction direction = Transport::Outgoing;
};

Transport::Transport(Manager *manager, const QDomElement &el) :
    XMPP::Jingle::Transport(manager),
    d(new Private)
{
    d->manager = manager;
    d->sid = el.attribute(QStringLiteral("sid"));
    d->direction = Transport::Incoming;
    // TODO remaining
    if (d->sid.isEmpty()) { // is invalid
        d.reset(); //  make invalid
    }
}

Transport::~Transport()
{

}

void Transport::start()
{

}

bool Transport::update(const QDomElement &el)
{
    Q_UNUSED(el)
    return false; // TODO
}

QDomElement Transport::takeUpdate(QDomDocument *doc)
{
    Q_UNUSED(doc)
    return QDomElement(); // TODO
}

bool Transport::isValid() const
{
    return d != nullptr;
}

QString Transport::sid() const
{
    return d->sid;
}

QSharedPointer<XMPP::Jingle::Transport> Transport::createOutgoing(Manager *manager)
{
    auto d = new Private;
    d->manager = manager;
    d->direction = Transport::Outgoing;
    d->mode = Transport::Tcp;
    do {
        d->sid = QString("s5b_%1").arg(qrand() & 0xffff, 4, 16, QChar('0')); // FIXME check for collisions
    } while (d->manager->hasTrasport(d->sid));

    auto t = new Transport;
    t->d.reset(d);
    return QSharedPointer<XMPP::Jingle::Transport>(t);
}

//----------------------------------------------------------------
// Manager
//----------------------------------------------------------------

class Manager::Private
{
public:
    XMPP::Jingle::Manager *jingleManager;

    // sid -> transport mapping
    QHash<QString,QSharedPointer<XMPP::Jingle::Transport>> transports;
};

Manager::Manager(XMPP::Jingle::Manager *manager) :
    TransportManager(manager),
    d(new Private)
{
    d->jingleManager = manager;
}

QSharedPointer<XMPP::Jingle::Transport> Manager::sessionInitiate()
{
    auto t = Transport::createOutgoing(this);
    d->transports.insert(t.staticCast<Transport>()->sid(), t);
    return t;
}

QSharedPointer<XMPP::Jingle::Transport> Manager::sessionInitiate(const QDomElement &transportEl)
{
    auto t = new Transport(this, transportEl);
    QSharedPointer<XMPP::Jingle::Transport> ret(t);
    if (t->isValid()) {
        d->transports.insert(t->sid(), ret);
    } else {
        ret.reset();
    }
    return ret;
}

bool Manager::hasTrasport(const QString &sid) const
{
    return d->transports.contains(sid);
}



} // namespace S5B
} // namespace Jingle
} // namespace XMPP
