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
#include "xmpp/jid/jid.h"
#include "xmpp_client.h"

namespace XMPP {
namespace Jingle {
namespace IBB {

class Connection : public XMPP::Jingle::Connection
{
    Q_OBJECT
public:
    Jid peer;
    QString sid;
    size_t blockSize;

    bool offerSent = false;
    bool offerReceived = false;

    Connection(const Jid &jid, const QString &sid, size_t blockSize) :
        peer(jid),
        sid(sid),
        blockSize(blockSize)
    {

    }

    void checkAndStartConnection()
    {
        // TODO
    }
};

struct Transport::Private
{
    Transport *q = nullptr;
    Pad::Ptr pad;
    QMap<QString,QSharedPointer<Connection>> connections;
    QList<QSharedPointer<Connection>> readyConnections;
    size_t defaultBlockSize = 4096;
    bool started = false;

    QSharedPointer<Connection> makeConnection(const Jid &jid, const QString &sid, size_t blockSize)
    {
        // TODO connect some signals
        return QSharedPointer<Connection>::create(jid, sid, blockSize);
    }
};

Transport::Transport(const TransportManagerPad::Ptr &pad) :
    d(new Private)
{
    d->q = this;
    d->pad = pad.staticCast<Pad>();
}

Transport::Transport(const TransportManagerPad::Ptr &pad, const QDomElement &transportEl) :
    d(new Private)
{
    d->q = this;
    d->pad = pad.staticCast<Pad>();
    update(transportEl);
    if (d->connections.isEmpty()) {
        d.reset();
        return;
    }
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
    if (d->connections.isEmpty()) { // seems like outgoing
        QString sid = d->pad->generateSid();
        auto conn = d->makeConnection(d->pad->session()->peer(), sid, d->defaultBlockSize);
        d->connections.insert(sid, conn);
    }
    emit updated();
}

void Transport::start()
{
    d->started = true;

    for (auto &c: d->connections) {
        c->checkAndStartConnection();
    }
}

bool Transport::update(const QDomElement &transportEl)
{
    QString sid = transportEl.attribute(QString::fromLatin1("sid"));
    if (sid.isEmpty()) {
        return false;
    }

    size_t bs_final = d->defaultBlockSize;
    auto bs = transportEl.attribute(QString::fromLatin1("block-size"));
    if (!bs.isEmpty()) {
        size_t bsn = bs.toULongLong();
        if (bsn && bsn <= bs_final) {
            bs_final = bsn;
        }
    }

    auto it = d->connections.find(sid);
    if (it == d->connections.end()) {
        if (!d->pad->registerSid(sid)) {
            return false; // TODO we need last error somewhere
        }
        it = d->connections.insert(sid, d->makeConnection(d->pad->session()->peer(), sid, bs_final));
    } else {
        if (bs_final < (*it)->blockSize) {
            (*it)->blockSize = bs_final;
        }
    }

    (*it)->offerReceived = true;
    if (d->started) {
        (*it)->checkAndStartConnection();
    }
    return true;
}

bool Transport::hasUpdates() const
{
    for (auto &c: d->connections) {
        if (!c->offerSent) {
            return true;
        }
    }
    return false;
}

OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate()
{
    OutgoingTransportInfoUpdate upd;
    if (!isValid()) {
        return upd;
    }

    QSharedPointer<Connection> connection;
    for (auto &c: d->connections) {
        if (!c->offerSent) {
            connection = c;
            break;
        }
    }
    if (!connection) {
        return upd;
    }

    auto doc = d->pad->session()->manager()->client()->doc();

    QDomElement tel = doc->createElementNS(NS, "transport");
    tel.setAttribute(QStringLiteral("sid"), connection->sid);
    tel.setAttribute(QString::fromLatin1("block-size"), qulonglong(connection->blockSize));

    upd = OutgoingTransportInfoUpdate{tel, [this, connection]() mutable {
        if (d->started)
            connection->checkAndStartConnection();
    }};

    connection->offerSent = true;
    return upd;
}

bool Transport::isValid() const
{
    return true;
}

Transport::Features Transport::features() const
{
    return AlwaysConnect | Reliable | Slow;
}

Connection::Ptr Transport::connection() const
{
    return d->readyConnections.takeLast();
}

size_t Transport::blockSize() const
{
    return d->defaultBlockSize;
}

Pad::Pad(Manager *manager, Session *session)
{
    _manager = manager;
    _session = session;
}

QString Pad::ns() const
{
    return NS;
}

Session *Pad::session() const
{
    return _session;
}

TransportManager *Pad::manager() const
{
    return _manager;
}

QString Pad::generateSid() const
{
    return _manager->generateSid(_session->peer());
}

bool Pad::registerSid(const QString &sid)
{
    return _manager->registerSid(_session->peer(), sid);
}

void Pad::forgetSid(const QString &sid)
{
    _manager->forgetSid(_session->peer(), sid);
}

struct Manager::Private
{
    QSet<QPair<Jid,QString>> sids;
};

Manager::Manager(QObject *parent) :
    TransportManager(parent),
    d(new Private)
{

}

QString Manager::generateSid(const Jid &remote)
{
    QString sid;
    QPair<Jid,QString> key;
    do {
        sid = QString("ibb_%1").arg(qrand() & 0xffff, 4, 16, QChar('0'));
        key = qMakePair(remote, sid);
    } while (d->sids.contains(key));

    d->sids.insert(key);
    return sid;
}

bool Manager::registerSid(const Jid &remote, const QString &sid)
{
    QPair<Jid,QString> key = qMakePair(remote, sid);
    if (d->sids.contains(key)) {
        return false;
    }
    d->sids.insert(key);
    return true;
}

void Manager::forgetSid(const Jid &remote, const QString &sid)
{
    QPair<Jid,QString> key = qMakePair(remote, sid);
    d->sids.remove(key);
}

} // namespace IBB
} // namespace Jingle
} // namespace XMPP
