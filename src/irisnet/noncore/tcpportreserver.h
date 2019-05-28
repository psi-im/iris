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

#ifndef TCPPORTRESERVER_H
#define TCPPORTRESERVER_H

#include <QObject>
#include <QSharedPointer>
#include <QVariant>

class QTcpServer;
class QHostAddress;

namespace XMPP {

class TcpPortScope;
/**
 * @brief The TcpPortDiscoverer class
 *
 * Discovers / starts listening on a set of unique tcp ports.
 */
class TcpPortDiscoverer : public QObject
{
    Q_OBJECT
public:
    enum PortType {
        Direct     = 0x1,
        NatAssited = 0x2,
        Tunneled   = 0x4
    };
    Q_DECLARE_FLAGS(PortTypes, PortType)

    struct Port
    {
        PortType portType;
        QSharedPointer<QTcpServer> server;
        QString  publishHost;
        quint16  publishPort;
        QVariant meta;
    };

    TcpPortDiscoverer(TcpPortScope *scope);
    bool setExternalHost(const QString &extHost, quint16 extPort, const QHostAddress &localIp, quint16 localPort);

    PortTypes inProgressPortTypes() const;
    QList<Port> takePorts();
public slots:
    void start(); // it's autocalled after outside worldis notified about this new discoverer
    void stop();
signals:
    void portAvailable();
private:
    TcpPortScope *scope = nullptr;
    QList<Port> ports;
};

class TcpPortReserver;
/**
 * @brief The TcpPortScope class
 *
 * Handles scopes of ports. For example just S5B dedicated ports.
 * There only on scope instance per scope id
 */
class TcpPortScope: public QObject
{
    Q_OBJECT
public:
    TcpPortScope(const QString &scopeId, TcpPortReserver *reserver);
    ~TcpPortScope();
    TcpPortDiscoverer* disco();
private:

    friend class TcpPortDiscoverer;
    QSharedPointer<QTcpServer> bind(const QHostAddress &addr, quint16 port);

private:
    class Private;
    QScopedPointer<Private> d;
};


/**
 * @brief The TcpPortReserver class
 * This class should have the only instance per application
 */
class TcpPortReserver : public QObject
{
    Q_OBJECT
public:
    explicit TcpPortReserver(QObject *parent = nullptr);
    ~TcpPortReserver();
    TcpPortScope *scopeFactory(const QString &id);
signals:
    void newDiscoverer(TcpPortDiscoverer *discoverer);

public slots:
};

} // namespace XMPP

Q_DECLARE_OPERATORS_FOR_FLAGS(XMPP::TcpPortDiscoverer::PortTypes)

#endif // TCPPORTRESERVER_H
