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

#include "ice176.h"

namespace XMPP {

class Ice176::Private : public QObject
{
	Q_OBJECT

public:
	Ice176 *q;

	Private(Ice176 *_q) :
		QObject(_q),
		q(_q)
	{
	}
};

Ice176::Ice176(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

Ice176::~Ice176()
{
	delete d;
}

void Ice176::reset()
{
	// TODO
}

void Ice176::setLocalAddresses(const QList<LocalAddress> &addrs)
{
	// TODO
	Q_UNUSED(addrs);
}

void Ice176::setStunService(StunServiceType type, const QHostAddress &addr, int port)
{
	// TODO
	Q_UNUSED(type);
	Q_UNUSED(addr);
	Q_UNUSED(port);
}

void Ice176::setComponentCount(int count)
{
	// TODO
	Q_UNUSED(count);
}

void Ice176::start(Mode mode)
{
	// TODO
	Q_UNUSED(mode);
}

QString Ice176::localUfrag() const
{
	// TODO
	return QString();
}

QString Ice176::localPassword() const
{
	// TODO
	return QString();
}

void Ice176::setPeerUfrag(const QString &ufrag)
{
	// TODO
	Q_UNUSED(ufrag);
}

void Ice176::setPeerPassword(const QString &pass)
{
	// TODO
	Q_UNUSED(pass);
}

void Ice176::addRemoteCandidates(const QList<Candidate> &list)
{
	// TODO
	Q_UNUSED(list);
}

bool Ice176::hasPendingDatagrams(int componentIndex) const
{
	// TODO
	Q_UNUSED(componentIndex);
	return false;
}

QByteArray Ice176::readDatagram(int componentIndex)
{
	// TODO
	Q_UNUSED(componentIndex);
	return QByteArray();
}

void Ice176::writeDatagram(const QByteArray &datagram, int componentIndex)
{
	// TODO
	Q_UNUSED(datagram);
	Q_UNUSED(componentIndex);
}

}

#include "ice176.moc"
