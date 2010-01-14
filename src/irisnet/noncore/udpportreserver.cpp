/*
 * Copyright (C) 2010  Barracuda Networks, Inc.
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

#include "udpportreserver.h"

#include <QUdpSocket>

namespace XMPP {

class UdpPortReserver::Private
{
public:
	class Item
	{
	public:
		int port; // reserved port, -1 if random
		QUdpSocket *sock; // whether we have a socket

		Item() :
			port(-1),
			sock(0)
		{
		}
	};

	UdpPortReserver *q;
	QList<Item> reserveList; // in order sorted by port
	QList<Item> borrowList;

	Private(UdpPortReserver *_q) :
		q(_q)
	{
	}

	bool add(int port)
	{
		Q_ASSERT(port > 0);
		Q_ASSERT(port < 65535);

		bool found = false;
		foreach(const Item &i, reserveList)
		{
			if(i.port == port)
			{
				found = true;
				break;
			}
		}
		Q_ASSERT(!found);

		int at = reserveList.count();

		Item i;
		i.port = port;
		reserveList += i;

		return ensureBind(&reserveList[at]);
	}

	bool ensureBind(Item *i)
	{
		if(i->sock)
			return true;

		QUdpSocket *sock = new QUdpSocket(q);
		if(!sock->bind(i->port))
		{
			delete sock;
			return false;
		}

		i->sock = sock;
		return true;
	}

	int findBorrowed(const QUdpSocket *sock) const
	{
		for(int n = 0; n < borrowList.count(); ++n)
		{
			if(borrowList[n].sock == sock)
				return n;
		}

		return -1;
	}

	QList<QUdpSocket*> borrowSockets(int count, QObject *parent)
	{
		// TODO
		Q_UNUSED(count);
		Q_UNUSED(parent);
		QList<QUdpSocket*> out;
		return out;
	}
};

UdpPortReserver::UdpPortReserver(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

UdpPortReserver::~UdpPortReserver()
{
	Q_ASSERT(d->borrowList.isEmpty());
	delete d;
}

bool UdpPortReserver::bind(int startPort, int len)
{
	QList<int> ports;
	for(int n = 0; n < len; ++n)
		ports += startPort + n;
	return bind(ports);
}

bool UdpPortReserver::bind(const QList<int> &ports)
{
	Q_ASSERT(d->reserveList.isEmpty());
	Q_ASSERT(d->borrowList.isEmpty());

	// make sure reserved ports are initially created in sorted order
	QList<int> sortedPorts = ports;
	qSort(sortedPorts);

	bool success = true;
	foreach(int port, sortedPorts)
	{
		if(!d->add(port))
			success = false;
	}

	return success;
}

QList<QUdpSocket*> UdpPortReserver::borrowSockets(int count, QObject *parent)
{
	return d->borrowSockets(count, parent);
}

void UdpPortReserver::returnSockets(const QList<QUdpSocket*> &sockList)
{
	foreach(QUdpSocket *sock, sockList)
	{
		int at = d->findBorrowed(sock);
		Q_ASSERT(at != -1);

		Private::Item i = d->borrowList.takeAt(at);
		if(i.port == -1)
		{
			i.sock->deleteLater();
			continue;
		}

		// put the item back in sorted order
		int insert_before = -1;
		for(int n = 0; n < d->reserveList.count(); ++n)
		{
			if(i.port < d->reserveList[n].port)
			{
				insert_before = n;
				break;
			}
		}

		d->reserveList.insert(insert_before, i);
	}
}

}
