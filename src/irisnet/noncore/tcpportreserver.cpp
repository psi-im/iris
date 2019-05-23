/*
 * tcpportreserver.cpp - dialog for handling tabbed chats
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

#include "tcpportreserver.h"
#include "ice176.h"

namespace XMPP {

struct StaticForwarding
{
    QString extHost;
    quint16 extPort;
    QHostAddress localIp;
    quint16 localPort;
};

struct TcpPortReserver::Private
{
    QMap<QString,StaticForwarding> staticForwarding;
};

TcpPortReserver::TcpPortReserver(QObject *parent) : QObject(parent),
  d(new Private)
{

}

TcpPortReserver::~TcpPortReserver()
{

}

QList<QTcpServer *> TcpPortReserver::borrow(const QString &scopeId, const QString &intanceId)
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

    return QList<QTcpServer *>(); // FIXME
}


void TcpPortReserver::setExternalHost(const QString &scopeId, const QString &extHost, quint16 extPort, const QHostAddress &localIp, quint16 localPort)
{
    d->staticForwarding.insert(scopeId, StaticForwarding{extHost, extPort, localIp, localPort});
}

} // namespace XMPP
