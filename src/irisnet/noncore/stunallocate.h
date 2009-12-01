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

class StunMessage;
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
		ErrorProtocol,
		ErrorCapacity,
		ErrorMismatch
	};

	StunAllocate(StunTransactionPool *pool);
	~StunAllocate();

	void setClientSoftwareNameAndVersion(const QString &str);

	void start();
	void stop();

	QString serverSoftwareNameAndVersion() const;

	QHostAddress reflexiveAddress() const;
	int reflexivePort() const;

	QHostAddress relayedAddress() const;
	int relayedPort() const;

	QList<QHostAddress> permissions() const;
	void setPermissions(const QList<QHostAddress> &perms);

	int packetHeaderOverhead(const QHostAddress &addr) const;

	QByteArray encode(const QByteArray &datagram, const QHostAddress &addr, int port);
	QByteArray decode(const QByteArray &encoded, QHostAddress *addr = 0, int *port = 0);
	QByteArray decode(const StunMessage &encoded, QHostAddress *addr = 0, int *port = 0);

	QString errorString() const;

signals:
	void started();
	void stopped();
	void error(XMPP::StunAllocate::Error e);

	// emitted after calling setPermissions()
	void permissionsChanged();

private:
	Q_DISABLE_COPY(StunAllocate)

	class Private;
	friend class Private;
	Private *d;
};

}

#endif
