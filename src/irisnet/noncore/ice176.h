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
		int network;
	};

	class Candidate
	{
	public:
		QString component;
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
	};

	Ice176(QObject *parent = 0);
	~Ice176();

	void reset();

	void setLocalAddresses(const QList<LocalAddress> &addrs);
	void setExternalAddress(const LocalAddress &base, const QHostAddress &ext, int port);
	void setStunService(StunServiceType type, const QHostAddress &addr, int port);
	void setComponentCount(int count);
	void setLocalCandidateTrickle(bool enabled); // default false

	void start(Mode mode);

	QString localUfrag() const;
	QString localPassword() const;

	void setPeerUfrag(const QString &ufrag);
	void setPeerPassword(const QString &pass);

	void addRemoteCandidates(const QList<Candidate> &list);

	bool hasPendingDatagrams(int componentIndex) const;
	QByteArray readDatagram(int componentIndex);
	void writeDatagram(const QByteArray &datagram, int componentIndex);

signals:
	void started();

	void localCandidatesReady(const QList<XMPP::Ice176::Candidate> &list);
	void componentReady(int index);

	void readyRead();
	void datagramsWritten(int count);

private:
	class Private;
	friend class Private;
	Private *d;
};

}

#endif
