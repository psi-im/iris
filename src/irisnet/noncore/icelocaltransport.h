/*
 * Copyright (C) 2009,2010  Barracuda Networks, Inc.
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
#include "icetransport.h"

class QHostAddress;
class QUdpSocket;

namespace QCA {
	class SecureArray;
}

namespace XMPP {

// this class manages a single port on a single interface, including the
//   relationship with an associated STUN/TURN server.  if TURN is used, this
//   class offers two paths (0=direct and 1=relayed), otherwise it offers
//   just one path (0=direct)
class IceLocalTransport : public IceTransport
{
	Q_OBJECT

public:
	enum StunServiceType
	{
		Auto,
		Basic,
		Relay
	};

	IceLocalTransport(QObject *parent = 0);
	~IceLocalTransport();

	void setClientSoftwareNameAndVersion(const QString &str);

	// passed socket must already be bind()'ed
	void start(QUdpSocket *sock);

	void setStunService(const QHostAddress &addr, int port, StunServiceType type = Auto);
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

	void addChannelPeer(const QHostAddress &addr, int port);

	// reimplemented
	virtual void stop();
	virtual bool hasPendingDatagrams(int path) const;
	virtual QByteArray readDatagram(int path, QHostAddress *addr, int *port);
	virtual void writeDatagram(int path, const QByteArray &buf, const QHostAddress &addr, int port);

signals:
	// may be emitted multiple times if Auto is used
	void stunFinished();

private:
	class Private;
	friend class Private;
	Private *d;
};

}

#endif
