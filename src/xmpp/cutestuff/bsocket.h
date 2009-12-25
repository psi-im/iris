/*
 * bsocket.h - QSocket wrapper based on Bytestream with SRV DNS support
 * Copyright (C) 2003  Justin Karneges
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
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


/*
	Socket with automatic hostname lookups, using SRV, AAAA and A DNS queries.

	Flow:
	1) SRV query for server
		: answer = host[]
		: failure -> (9)
		2) Primary query for host[i] (usually AAAA)
			: answer = address[]
			: failure -> (5)
			3) Connect to address[j]
				: connect -> FINISHED
				: failure -> j++, (3)
			4) address[] empty -> (5)
		5) Fallback query for host[i] (usually A)
			: answer = address[]
			: failure -> i++, (2)
			6) Connect to address[j]
			: connect -> FINISHED
			: failure -> j++, (6)
			7) address[] empty -> i++, (2)
		8) host[] empty -> (9)
	9) Try servername directly
*/
class BSocket : public ByteStream
{
	Q_OBJECT
public:
	enum Error { ErrConnectionRefused = ErrCustom, ErrHostNotFound };
	enum State { Idle, HostLookup, Connecting, Connected, Closing };
	BSocket(QObject *parent=0);
	~BSocket();

	void connectToHost(const QHostAddress &address, quint16 port);
	void connectToHost(const QString &host, quint16 port, QAbstractSocket::NetworkLayerProtocol protocol = QAbstractSocket::UnknownNetworkLayerProtocol);
	void connectToHost(const QString &service, const QString &transport, const QString &domain, quint16 port = std::numeric_limits<int>::max());
	int socket() const;
	void setSocket(int);
	int state() const;

	// from ByteStream
	bool isOpen() const;
	void close();
	void write(const QByteArray &);
	QByteArray read(int bytes=0);
	int bytesAvailable() const;
	int bytesToWrite() const;

	// local
	QHostAddress address() const;
	quint16 port() const;

	// remote
	QHostAddress peerAddress() const;
	quint16 peerPort() const;

signals:
	void hostFound();
	void connected();

private slots:
	void qs_hostFound();
	void qs_connected();
	void qs_closed();
	void qs_readyRead();
	void qs_bytesWritten(qint64);
	void qs_error(QAbstractSocket::SocketError);

	void handle_dns_ready(const QHostAddress&, quint16);
	void handle_dns_error(XMPP::ServiceResolver::Error e);
	void handle_connect_error(QAbstractSocket::SocketError);

private:
	class Private;
	Private *d;

	void reset(bool clear=false);
	void ensureSocket();
	void recreate_resolver();
	bool check_protocol_fallback();
	void dns_srv_try_next();
	bool connect_host_try_next();
};

// CS_NAMESPACE_END

#endif
