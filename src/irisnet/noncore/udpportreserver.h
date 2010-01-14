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

#ifndef UDPPORTRESERVER_H
#define UDPPORTRESERVER_H

#include <QObject>
#include <QList>

class QUdpSocket;

namespace XMPP {

// tries to bind to many ports in advance.  then sockets can be borrowed from
//   this object and returned as necessary.  if more sockets are borrowed than
//   are reserved, then random ports will be made.

// note: you must return all sockets before destructing
class UdpPortReserver : public QObject
{
	Q_OBJECT

public:
	UdpPortReserver(QObject *parent = 0);
	~UdpPortReserver();

	// returns false if not all ports could be immediately bound to.
	//   note that this is not fatal.  the ports that did succeed will be
	//   bound.  even if no bindings succeeded at all, you can still get
	//   random ports.
	bool bind(int startPort, int len);
	bool bind(const QList<int> &ports);

	// may return less than asked for, if we had no reserved ports left
	//   and we couldn't even bind random ones on the fly.  it attempts
	//   to return consecutive port values.  it will also attempt to bind
	//   again to ports that we couldn't get during the initial bind()
	//   call.
	QList<QUdpSocket*> borrowSockets(int count, QObject *parent = 0);
	void returnSockets(const QList<QUdpSocket*> &sockList);

private:
	class Private;
	Private *d;
};

}

#endif
