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

#ifndef STUNALLOCATE_H
#define STUNALLOCATE_H

#include <QObject>
#include <QList>

class QByteArray;
class QHostAddress;

namespace XMPP {

class StunTransactionPool;

class StunAllocate : public QObject
{
	Q_OBJECT

public:
	enum Error
	{
		ErrorGeneric,
		ErrorTimeout,
		ErrorRejected,
		ErrorProtocol
	};

	StunAllocate(StunTransactionPool *pool);
	~StunAllocate();

	void start();
	void stop();

	QHostAddress reflexiveAddress() const;
	int reflexivePort() const;

	QHostAddress relayedAddress() const;
	int relayedPort() const;

	QList<QHostAddress> permissions() const;
	void setPermissions(const QList<QHostAddress> &perms);

	bool hasPendingDatagrams() const;
	QByteArray readDatagram(QHostAddress *addr = 0, int *port = 0);
	void writeDatagram(const QByteArray &datagram, const QHostAddress &addr, int port);

signals:
	void started();
	void stopped();
	void error(XMPP::StunAllocate::Error e);

	// emitted after calling setPermissions()
	void permissionsChanged();

	void readyRead();
	void datagramsWritten(int count);

private:
	Q_DISABLE_COPY(StunAllocate)

	class Private;
	friend class Private;
	Private *d;
};

}

#endif
