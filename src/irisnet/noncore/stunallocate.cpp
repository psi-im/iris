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

#include "stunallocate.h"

#include <QMetaType>
#include <QHostAddress>
#include "stuntransaction.h"

Q_DECLARE_METATYPE(XMPP::StunAllocate::Error)

namespace XMPP {

class StunAllocate::Private : public QObject
{
	Q_OBJECT

public:
	StunAllocate *q;
	StunTransactionPool *pool;

	Private(StunAllocate *_q) :
		QObject(_q),
		q(_q),
		pool(0)
	{
		qRegisterMetaType<StunAllocate::Error>();
	}
};

StunAllocate::StunAllocate(StunTransactionPool *pool) :
	QObject(pool)
{
	d = new Private(this);
	d->pool = pool;
}

StunAllocate::~StunAllocate()
{
	delete d;
}

void StunAllocate::start()
{
	// TODO
	QMetaObject::invokeMethod(this, "error", Qt::QueuedConnection, Q_ARG(XMPP::StunAllocate::Error, ErrorGeneric));
}

void StunAllocate::stop()
{
	// TODO
}

QHostAddress StunAllocate::reflexiveAddress() const
{
	// TODO
	return QHostAddress();
}

int StunAllocate::reflexivePort() const
{
	// TODO
	return 0;
}

QHostAddress StunAllocate::relayedAddress() const
{
	// TODO
	return QHostAddress();
}

int StunAllocate::relayedPort() const
{
	// TODO
	return 0;
}

QList<QHostAddress> StunAllocate::permissions() const
{
	// TODO
	return QList<QHostAddress>();
}

void StunAllocate::setPermissions(const QList<QHostAddress> &perms)
{
	// TODO
	Q_UNUSED(perms);
}

QByteArray StunAllocate::encode(const QByteArray &datagram, const QHostAddress &addr, int port)
{
	// TODO
	Q_UNUSED(datagram);
	Q_UNUSED(addr);
	Q_UNUSED(port);
	return QByteArray();
}

QByteArray StunAllocate::decode(const QByteArray &encoded, QHostAddress *addr, int *port)
{
	// TODO
	Q_UNUSED(encoded);
	Q_UNUSED(addr);
	Q_UNUSED(port);
	return QByteArray();
}

QByteArray StunAllocate::decode(const StunMessage &encoded, QHostAddress *addr, int *port)
{
	// TODO
	Q_UNUSED(encoded);
	Q_UNUSED(addr);
	Q_UNUSED(port);
	return QByteArray();
}

}

#include "stunallocate.moc"
