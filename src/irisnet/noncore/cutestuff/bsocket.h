/*
 * bsocket.h - QSocket wrapper based on Bytestream with SRV DNS support
 * Copyright (C) 2003  Justin Karneges
 * Copyright (C) 2009-2010  Dennis Schridde
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

#ifndef CS_BSOCKET_H
#define CS_BSOCKET_H

#include <QAbstractSocket>
#include <limits>

#include "bytestream.h"
#include "netnames.h"

class QString;
class QObject;
class QByteArray;

// CS_NAMESPACE_BEGIN


/*!
    Socket with automatic hostname lookups, using SRV, AAAA and A DNS queries.
*/
class BSocket : public ByteStream
{
    Q_OBJECT
public:
    enum Error { ErrConnectionRefused = ErrCustom, ErrHostNotFound };
    enum State { Idle, HostLookup, Connecting, Connected, Closing };
    BSocket(QObject *parent=nullptr);
    ~BSocket();

    /*! Connect to an already resolved host */
    void connectToHost(const QHostAddress &address, quint16 port);
    /*! Connect to a host via the specified protocol, or the default protocols if not specified */
    void connectToHost(const QString &host, quint16 port, QAbstractSocket::NetworkLayerProtocol protocol = QAbstractSocket::UnknownNetworkLayerProtocol);
    /*! Connect to the hosts for the specified service */
    void connectToHost(const QString &service, const QString &transport, const QString &domain, quint16 port = std::numeric_limits<quint16>::max());
    virtual QAbstractSocket* abstractSocket() const;
    qintptr socket() const;
    void setSocket(QTcpSocket *);
    int state() const;

    // from ByteStream
    bool isOpen() const;
    void close();

    qint64 bytesAvailable() const;
    qint64 bytesToWrite() const;

    // local
    QHostAddress address() const;
    quint16 port() const;

    // remote
    QHostAddress peerAddress() const;
    quint16 peerPort() const;

protected:
    qint64 writeData(const char *data, qint64 maxSize);
    qint64 readData(char *data, qint64 maxSize);

signals:
    void hostFound();
    void connected();

private slots:
    void qs_connected();
    void qs_closed();
    void qs_readyRead();
    void qs_bytesWritten(qint64);
    void qs_error(QAbstractSocket::SocketError);

private:
    class Private;
    Private *d;

    void resetConnection(bool clear=false);
    void ensureConnector();
    void recreate_resolver();
    bool check_protocol_fallback();
    void dns_srv_try_next();
    bool connect_host_try_next();
    void qs_connected_step2(bool signalConnected = true);
};

// CS_NAMESPACE_END

#endif
