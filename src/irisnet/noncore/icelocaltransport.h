/*
 * Copyright (C) 2009  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#ifndef ICELOCALTRANSPORT_H
#define ICELOCALTRANSPORT_H

#include <QObject>
#include <QByteArray>

class QHostAddress;

namespace QCA {
	class SecureArray;
}

namespace XMPP {

// this class manages a single port on a single interface, including the
//   relationship with an associated STUN/TURN server.  if TURN is used, this
//   class offers two UDP channels (direct or relayed), otherwise it offers
//   just one UDP channel (direct).
class IceLocalTransport : public QObject
{
	Q_OBJECT

public:
	enum Error
	{
		ErrorGeneric
	};

	enum StunServiceType
	{
		Basic,
		Relay
	};

	enum TransmitPath
	{
		Direct,
		Relayed
	};

	IceLocalTransport(QObject *parent = 0);
	~IceLocalTransport();

	void start(const QHostAddress &addr, int port = -1);
	void stop();

	void setStunService(StunServiceType type, const QHostAddress &addr, int port);
	void setStunUsername(const QString &user);
	void setStunPassword(const QCA::SecureArray &pass);

	// obtain relay / reflexive
	void stunStart();

	QHostAddress localAddress() const;
	int localPort() const;

	QHostAddress serverReflexiveAddress() const;
	int serverReflexivePort() const;

	QHostAddress relayedAddress() const;
	int relayedPort() const;

	bool hasPendingDatagrams(TransmitPath path) const;
	QByteArray readDatagram(TransmitPath path, QHostAddress *addr, int *port);
	void writeDatagram(TransmitPath path, const QByteArray &buf, const QHostAddress &addr, int port);

signals:
	void started();
	void stopped();
	void stunFinished();
	void error(XMPP::IceLocalTransport::Error e);

	void readyRead(XMPP::IceLocalTransport::TransmitPath path);
	void datagramsWritten(XMPP::IceLocalTransport::TransmitPath path, int count);

private:
	class Private;
	friend class Private;
	Private *d;
};

}

#endif
