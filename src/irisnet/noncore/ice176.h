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

#ifndef ICE176_H
#define ICE176_H

#include <QObject>
#include <QString>
#include <QHostAddress>

namespace QCA {
	class SecureArray;
}

namespace XMPP {

class Ice176 : public QObject
{
	Q_OBJECT

public:
	enum Mode
	{
		Initiator,
		Responder
	};

	enum StunServiceType
	{
		Basic,
		Relay
	};

	class LocalAddress
	{
	public:
		QHostAddress addr;
		int network; // -1 = unknown
		bool isVpn;

		LocalAddress() :
			network(-1),
			isVpn(false)
		{
		}

		bool operator==(const LocalAddress &other) const
		{
			if(addr == other.addr && network == other.network)
				return true;
			else
				return false;
		}

		inline bool operator!=(const LocalAddress &other) const
		{
			return !operator==(other);
		}
	};

	class ExternalAddress
	{
	public:
		LocalAddress base;
		QHostAddress addr;
		int portBase; // -1 = same as base

		ExternalAddress() :
			portBase(-1)
		{
		}
	};

	class Candidate
	{
	public:
		int component;
		QString foundation;
		int generation;
		QString id;
		QHostAddress ip;
		int network;
		int port;
		int priority;
		QString protocol;
		QHostAddress rel_addr;
		int rel_port;
		QHostAddress rem_addr;
		int rem_port;
		QString type;

		Candidate() :
			component(-1),
			generation(-1),
			network(-1),
			port(-1),
			priority(-1),
			rel_port(-1),
			rem_port(-1)
		{
		}
	};

	Ice176(QObject *parent = 0);
	~Ice176();

	void reset();

	// default = -1 (unspecified)
	// if a base port is specified, it is only considered for the initial
	//   component count.  if components are later added, random ports
	//   will be used.
	void setBasePort(int port);

	void setLocalAddresses(const QList<LocalAddress> &addrs);

	// one per local address.  you must set local addresses first.
	void setExternalAddresses(const QList<ExternalAddress> &addrs);

	void setStunService(StunServiceType type, const QHostAddress &addr, int port);

	void setComponentCount(int count);
	void setLocalCandidateTrickle(bool enabled); // default false

	void start(Mode mode);

	QString localUfrag() const;
	QString localPassword() const;

	void setPeerUfrag(const QString &ufrag);
	void setPeerPassword(const QString &pass);

	void setStunUsername(const QString &user);
	void setStunPassword(const QCA::SecureArray &pass);

	void addRemoteCandidates(const QList<Candidate> &list);

	bool hasPendingDatagrams(int componentIndex) const;
	QByteArray readDatagram(int componentIndex);
	void writeDatagram(int componentIndex, const QByteArray &datagram);

signals:
	void started();

	void localCandidatesReady(const QList<XMPP::Ice176::Candidate> &list);
	void componentReady(int index);

	void readyRead(int componentIndex);
	void datagramsWritten(int componentIndex, int count);

private:
	class Private;
	friend class Private;
	Private *d;
};

}

#endif
