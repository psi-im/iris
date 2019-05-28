/*
 * tcpportreserver.cpp - a utility to bind local tcp server sockets
 * Copyright (C) 2019  Sergey Ilinykh
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>

#include "tcpportreserver.h"
#include "ice176.h"

namespace XMPP {

TcpPortDiscoverer::TcpPortDiscoverer(TcpPortScope *scope) :
    QObject(scope),
    scope(scope)
{

}

bool TcpPortDiscoverer::setExternalHost(const QString &extHost, quint16 extPort, const QHostAddress &localAddr, quint16 localPort)
{
    auto server = scope->bind(localAddr, localPort);
    if (!server) {
        return false;
    }
    Port p;
    p.portType = NatAssited;
    p.server = server;
    p.publishHost = extHost;
    p.publishPort = extPort;
    ports.append(p);
    return true;
}

TcpPortDiscoverer::PortTypes TcpPortDiscoverer::inProgressPortTypes() const
{
    return 0; // same as for stop()
}

void TcpPortDiscoverer::start()
{
    QList<QHostAddress> listenAddrs;
    foreach(const QNetworkInterface &ni, QNetworkInterface::allInterfaces())
    {
        if (!(ni.flags() & (QNetworkInterface::IsUp | QNetworkInterface::IsRunning))) {
            continue;
        }
        if (ni.flags() & QNetworkInterface::IsLoopBack) {
            continue;
        }
        QList<QNetworkAddressEntry> entries = ni.addressEntries();
        foreach(const QNetworkAddressEntry &na, entries)
        {
            QHostAddress h = na.ip();
            if (h.isLoopback()) {
                continue;
            }

            // don't put the same address in twice.
            //   this also means that if there are
            //   two link-local ipv6 interfaces
            //   with the exact same address, we
            //   only use the first one
            if(listenAddrs.contains(h))
                continue;
#if QT_VERSION >= QT_VERSION_CHECK(5,11,0)
            if(h.protocol() == QAbstractSocket::IPv6Protocol && h.isLinkLocal())
#else
            if(h.protocol() == QAbstractSocket::IPv6Protocol && XMPP::Ice176::isIPv6LinkLocalAddress(h))
#endif
                h.setScopeId(ni.name());
            listenAddrs += h;
        }
    }

    for (auto &h: listenAddrs) {
        auto server = scope->bind(h, 0);
        if (!server) {
            continue;
        }
        Port p;
        p.portType = Direct;
        p.server = server;
        p.publishHost = server->serverAddress().toString();
        p.publishPort = server->serverPort();
        ports.append(p);
    }
}

void TcpPortDiscoverer::stop()
{
    // nothing really to do here. but if we invent extension interface it can call stop on subdisco
}

QList<TcpPortDiscoverer::Port> TcpPortDiscoverer::takePorts()
{
    auto ret = ports;
    ports.clear();
    for (auto &p: ret) {
        p.server->disconnect(this);
    }
    return ret;
}

// --------------------------------------------------------------------------
// TcpPortScope
// --------------------------------------------------------------------------
struct TcpPortScope::Private
{
    QString id;
    QHash<QPair<QHostAddress,quint16>, QWeakPointer<QTcpServer>> servers;
};


TcpPortScope::TcpPortScope(const QString &scopeId, TcpPortReserver *reserver) :
    QObject(reserver),
    d(new Private)
{
    d->id = scopeId;
}

TcpPortScope::~TcpPortScope()
{

}

TcpPortDiscoverer *TcpPortScope::disco()
{
    auto discoverer = new TcpPortDiscoverer(this);
    QMetaObject::invokeMethod(parent(), "newDiscoverer", Q_ARG(TcpPortDiscoverer*, discoverer));
    QMetaObject::invokeMethod(discoverer, "start", Qt::QueuedConnection, Q_ARG(TcpPortDiscoverer*, discoverer));
    return discoverer;
}

QSharedPointer<QTcpServer> TcpPortScope::bind(const QHostAddress &addr, quint16 port)
{
    if (port) {
        auto srv = d->servers.value(qMakePair(addr,port)).toStrongRef();
        if (srv) {
            return srv;
        }
    }
    auto s = new QTcpServer(this);
    if (!s->listen(addr, port)) {
        delete s;
        return QSharedPointer<QTcpServer>();
    }

    QSharedPointer<QTcpServer> shared(s, [](QTcpServer *s){
        auto scope = qobject_cast<TcpPortScope*>(s->parent());
        scope->d->servers.remove(qMakePair(s->serverAddress(),s->serverPort()));
    });
    d->servers.insert(qMakePair(s->serverAddress(), s->serverPort()), shared.toWeakRef());

    return shared;
}


// --------------------------------------------------------------------------
// TcpPortScope
// --------------------------------------------------------------------------
TcpPortReserver::TcpPortReserver(QObject *parent) : QObject(parent)
{

}

TcpPortReserver::~TcpPortReserver()
{

}

TcpPortScope *TcpPortReserver::scopeFactory(const QString &id)
{
    auto scope = findChild<TcpPortScope*>(id, Qt::FindDirectChildrenOnly);
    if (!scope) {
        scope = new TcpPortScope(id, this);
        scope->setParent(this);
    }
    return scope;
}

} // namespace XMPP
