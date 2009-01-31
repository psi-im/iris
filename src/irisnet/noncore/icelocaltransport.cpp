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

#include "icelocaltransport.h"

#include <QHostAddress>
#include <QUdpSocket>
#include <QtCrypto>
#include "objectsession.h"
#include "stunmessage.h"
#include "stuntransaction.h"
#include "stunbinding.h"
#include "stunallocate.h"

// don't queue more incoming packets than this per transmit path
#define MAX_PACKET_QUEUE 64

namespace XMPP {

//----------------------------------------------------------------------------
// SafeUdpSocket
//----------------------------------------------------------------------------
// DOR-safe wrapper for QUdpSocket
class SafeUdpSocket : public QObject
{
	Q_OBJECT

private:
	ObjectSession sess;
	QUdpSocket *sock;
	int writtenCount;

public:
	SafeUdpSocket(QObject *parent = 0) :
		QObject(parent),
		sess(this)
	{
		sock = new QUdpSocket(this);
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
		connect(sock, SIGNAL(bytesWritten(qint64)), SLOT(sock_bytesWritten(qint64)));

		writtenCount = 0;
	}

	~SafeUdpSocket()
	{
		sock->disconnect(this);
		sock->setParent(0);
		sock->deleteLater();
	}

	bool bind(const QHostAddress &addr, quint16 port = 0)
	{
		return sock->bind(addr, port);
	}

	quint16 localPort() const
	{
		return sock->localPort();
	}

	bool hasPendingDatagrams() const
	{
		return sock->hasPendingDatagrams();
	}

	QByteArray readDatagram(QHostAddress *address = 0, quint16 *port = 0)
	{
		if(!sock->hasPendingDatagrams())
			return QByteArray();

		QByteArray buf;
		buf.resize(sock->pendingDatagramSize());
		sock->readDatagram(buf.data(), buf.size(), address, port);
		return buf;
	}

	void writeDatagram(const QByteArray &buf, const QHostAddress &address, quint16 port)
	{
		sock->writeDatagram(buf, address, port);
	}

signals:
	void readyRead();
	void datagramsWritten(int count);

private slots:
	void sock_readyRead()
	{
		emit readyRead();
	}

	void sock_bytesWritten(qint64 bytes)
	{
		Q_UNUSED(bytes);

		++writtenCount;
		sess.deferExclusive(this, "processWritten");
	}

	void processWritten()
	{
		int count = writtenCount;
		writtenCount = 0;

		emit datagramsWritten(count);
	}
};

//----------------------------------------------------------------------------
// IceLocalTransport
//----------------------------------------------------------------------------
class IceLocalTransport::Private : public QObject
{
	Q_OBJECT

public:
	enum WriteType
	{
		InternalWrite,
		DirectWrite,
		RelayedWrite
	};

	class Datagram
	{
	public:
		QHostAddress addr;
		int port;
		QByteArray buf;
	};

	IceLocalTransport *q;
	ObjectSession sess;
	SafeUdpSocket *sock;
	StunTransactionPool *pool;
	StunBinding *stunBinding;
	StunAllocate *stunAllocate;
	bool alloc_started;
	QHostAddress addr;
	int port;
	QHostAddress refAddr;
	int refPort;
	QHostAddress relAddr;
	int relPort;
	QHostAddress stunAddr;
	int stunPort;
	IceLocalTransport::StunServiceType stunType;
	QString stunUser;
	QCA::SecureArray stunPass;
	QList<Datagram> in;
	QList<Datagram> inRelayed;
	QList<Datagram> outRelayed;
	QList<WriteType> pendingWrites;

	Private(IceLocalTransport *_q) :
		QObject(_q),
		q(_q),
		sess(this),
		sock(0),
		stunBinding(0),
		stunAllocate(0),
		alloc_started(false),
		port(-1),
		refPort(-1),
		relPort(-1)
	{
		pool = new StunTransactionPool(StunTransaction::Udp, this);
		connect(pool, SIGNAL(retransmit(XMPP::StunTransaction *)), SLOT(pool_retransmit(XMPP::StunTransaction *)));
	}

	~Private()
	{
		reset();
	}

	void reset()
	{
		sess.reset();

		delete stunBinding;
		stunBinding = 0;

		delete stunAllocate;
		stunAllocate = 0;
		alloc_started = false;

		delete sock;
		sock = 0;

		addr = QHostAddress();
		port = -1;

		refAddr = QHostAddress();
		refPort = -1;

		relAddr = QHostAddress();
		relPort = -1;

		in.clear();
		inRelayed.clear();
		outRelayed.clear();
		pendingWrites.clear();
	}

	void start()
	{
		Q_ASSERT(!sock);

		sock = new SafeUdpSocket(this);
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
		connect(sock, SIGNAL(datagramsWritten(int)), SLOT(sock_datagramsWritten(int)));

		sess.defer(q, "postStart");
	}

	void stop()
	{
		Q_ASSERT(sock);

		if(stunAllocate)
			stunAllocate->stop();
		else
			sess.defer(q, "postStop");
	}

	void stunStart()
	{
		Q_ASSERT(!stunBinding && !stunAllocate);

		if(stunType == IceLocalTransport::Relay)
		{
			stunAllocate = new StunAllocate(pool);
			connect(stunAllocate, SIGNAL(started()), SLOT(allocate_started()));
			connect(stunAllocate, SIGNAL(stopped()), SLOT(allocate_stopped()));
			connect(stunAllocate, SIGNAL(error(XMPP::StunAllocate::Error)), SLOT(allocate_error(XMPP::StunAllocate::Error)));
			connect(stunAllocate, SIGNAL(permissionsChanged()), SLOT(allocate_permissionsChanged()));
			connect(stunAllocate, SIGNAL(readyRead()), SLOT(allocate_readyRead()));
			connect(stunAllocate, SIGNAL(datagramsWritten(int)), SLOT(allocate_datagramsWritten(int)));
			stunAllocate->start();
		}
		else // Basic
		{
			stunBinding = new StunBinding(pool);
			connect(stunBinding, SIGNAL(success()), SLOT(binding_success()));
			connect(stunBinding, SIGNAL(error(XMPP::StunBinding::Error)), SLOT(binding_error(XMPP::StunBinding::Error)));
			stunBinding->start();
		}
	}

	void writeRelayed(const QByteArray &buf, const QHostAddress &addr, int port)
	{
		Datagram dg;
		dg.addr = addr;
		dg.port = port;
		dg.buf = buf;

		// TODO: if permission is ready, send.  else, queue packet,
		//   request permission, flush queue when done
	}

	void processIncomingStun(const QByteArray &buf)
	{
		StunMessage message = StunMessage::fromBinary(buf);
		if(message.isNull())
		{
			printf("Warning: server responded with what doesn't seem to be a STUN packet, skipping.\n");
			return;
		}

		if(!pool->writeIncomingMessage(message))
		{
			printf("Warning: received unexpected message, skipping.\n");
		}
	}

public slots:
	void postStart()
	{
		bool ok;
		if(port != -1)
			ok = sock->bind(addr, port);
		else
			ok = sock->bind(addr, 0);

		if(ok)
		{
			port = sock->localPort();
			emit q->started();
		}
		else
		{
			reset();
			emit q->error(IceLocalTransport::ErrorGeneric);
		}
	}

	void postStop()
	{
		reset();
		emit q->stopped();
	}

	void sock_readyRead()
	{
		ObjectSessionWatcher watcher(&sess);

		QList<Datagram> dreads;
		//QList<Datagram> rreads;

		while(sock->hasPendingDatagrams())
		{
			QHostAddress from;
			quint16 fromPort;
			QByteArray buf = sock->readDatagram(&from, &fromPort);

			if(from == stunAddr && fromPort == stunPort)
			{
				processIncomingStun(buf);
				if(!watcher.isValid())
					return;
			}
			else
			{
				Datagram dg;
				dg.addr = from;
				dg.port = fromPort;
				dg.buf = buf;
				dreads += dg;
			}
		}

		if(dreads.count() > 0)
		{
			in += dreads;
			emit q->readyRead(IceLocalTransport::Direct);
		}

		// TODO: emit q->readyRead(IceLocalTransport::Relayed);
	}

	void sock_datagramsWritten(int count)
	{
		Q_ASSERT(count <= pendingWrites.count());

		int dwrites = 0;
		int rwrites = 0;
		for(int n = 0; n < count; ++n)
		{
			WriteType type = pendingWrites.takeFirst();
			if(type == DirectWrite)
				++dwrites;
			else if(type == RelayedWrite)
				++rwrites;
		}

		ObjectSessionWatcher watch(&sess);

		if(dwrites > 0)
		{
			emit q->datagramsWritten(IceLocalTransport::Direct, dwrites);
			if(!watch.isValid())
				return;
		}

		if(rwrites > 0)
			emit q->datagramsWritten(IceLocalTransport::Relayed, rwrites);
	}

	void pool_retransmit(XMPP::StunTransaction *trans)
	{
		// warning: read StunTransactionPool docs before modifying
		//   this function

		pendingWrites += InternalWrite;
		sock->writeDatagram(trans->packet(), stunAddr, stunPort);
	}

	void binding_success()
	{
		refAddr = stunBinding->reflexiveAddress();
		refPort = stunBinding->reflexivePort();

		delete stunBinding;
		stunBinding = 0;

		emit q->stunFinished();
	}

	void binding_error(XMPP::StunBinding::Error e)
	{
		Q_UNUSED(e);

		delete stunBinding;
		stunBinding = 0;

		emit q->stunFinished();
	}

	void allocate_started()
	{
		refAddr = stunAllocate->reflexiveAddress();
		refPort = stunAllocate->reflexivePort();
		relAddr = stunAllocate->relayedAddress();
		relPort = stunAllocate->relayedPort();
		alloc_started = true;

		emit q->stunFinished();
	}

	void allocate_stopped()
	{
		// allocation deleted
		delete stunAllocate;
		stunAllocate = 0;
		alloc_started = false;

		postStop();
	}

	void allocate_error(XMPP::StunAllocate::Error e)
	{
		delete stunAllocate;
		stunAllocate = 0;
		bool wasStarted = alloc_started;
		alloc_started = false;

		// this means our relay died on us.  in the future we might
		//   consider reporting this
		if(wasStarted)
			return;

		// if we get an error during initialization, fall back to
		//   binding
		if(e != StunAllocate::ErrorTimeout)
		{
			stunType = IceLocalTransport::Basic;
			stunStart();
		}
		else
		{
			emit q->stunFinished();
		}
	}

	void allocate_permissionsChanged()
	{
		// TODO
	}

	void allocate_readyRead()
	{
		// TODO
	}

	void allocate_datagramsWritten(int count)
	{
		// TODO
		Q_UNUSED(count);
	}
};

IceLocalTransport::IceLocalTransport(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

IceLocalTransport::~IceLocalTransport()
{
	delete d;
}

void IceLocalTransport::start(const QHostAddress &addr, int port)
{
	d->addr = addr;
	d->port = port;
	d->start();
}

void IceLocalTransport::stop()
{
	d->stop();
}

void IceLocalTransport::setStunService(StunServiceType type, const QHostAddress &addr, int port)
{
	d->stunType = type;
	d->stunAddr = addr;
	d->stunPort = port;
}

void IceLocalTransport::setStunUsername(const QString &user)
{
	d->stunUser = user;
}

void IceLocalTransport::setStunPassword(const QCA::SecureArray &pass)
{
	d->stunPass = pass;
}

void IceLocalTransport::stunStart()
{
	d->stunStart();
}

QHostAddress IceLocalTransport::localAddress() const
{
	return d->addr;
}

int IceLocalTransport::localPort() const
{
	return d->port;
}

QHostAddress IceLocalTransport::serverReflexiveAddress() const
{
	return d->refAddr;
}

int IceLocalTransport::serverReflexivePort() const
{
	return d->refPort;
}

QHostAddress IceLocalTransport::relayedAddress() const
{
	return d->relAddr;
}

int IceLocalTransport::relayedPort() const
{
	return d->relPort;
}

bool IceLocalTransport::hasPendingDatagrams(TransmitPath path) const
{
	if(path == Direct)
		return !d->in.isEmpty();
	else if(path == Relayed)
		return !d->inRelayed.isEmpty();
	else
	{
		Q_ASSERT(0);
		return false;
	}
}

QByteArray IceLocalTransport::readDatagram(TransmitPath path, QHostAddress *addr, int *port)
{
	QList<Private::Datagram> *in = 0;
	if(path == Direct)
		in = &d->in;
	else if(path == Relayed)
		in = &d->inRelayed;
	else
		Q_ASSERT(0);

	if(!in->isEmpty())
	{
		Private::Datagram datagram = in->takeFirst();
		*addr = datagram.addr;
		*port = datagram.port;
		return datagram.buf;
	}
	else
	{
		*addr = QHostAddress();
		*port = -1;
		return QByteArray();
	}
}

void IceLocalTransport::writeDatagram(TransmitPath path, const QByteArray &buf, const QHostAddress &addr, int port)
{
	if(path == Direct)
	{
		d->pendingWrites += Private::DirectWrite;
		d->sock->writeDatagram(buf, addr, port);
	}
	else if(path == Relayed)
	{
		if(d->stunAllocate && d->alloc_started)
			d->writeRelayed(buf, addr, port);
	}
	else
		Q_ASSERT(0);
}

}

#include "icelocaltransport.moc"
