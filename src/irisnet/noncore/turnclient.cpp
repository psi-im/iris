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

#include "turnclient.h"

#include <QtCrypto>
#include "stunmessage.h"
#include "stuntransaction.h"
#include "stunallocate.h"
#include "objectsession.h"
#include "bytestream.h"
#include "bsocket.h"
#include "httpconnect.h"
#include "socks.h"

namespace XMPP {

//----------------------------------------------------------------------------
// TurnClient::Proxy
//----------------------------------------------------------------------------
TurnClient::Proxy::Proxy()
{
	t = None;
}

TurnClient::Proxy::~Proxy()
{
}

int TurnClient::Proxy::type() const
{
	return t;
}

QString TurnClient::Proxy::host() const
{
	return v_host;
}

quint16 TurnClient::Proxy::port() const
{
	return v_port;
}

QString TurnClient::Proxy::user() const
{
	return v_user;
}

QString TurnClient::Proxy::pass() const
{
	return v_pass;
}

void TurnClient::Proxy::setHttpConnect(const QString &host, quint16 port)
{
	t = HttpConnect;
	v_host = host;
	v_port = port;
}

void TurnClient::Proxy::setSocks(const QString &host, quint16 port)
{
	t = Socks;
	v_host = host;
	v_port = port;
}

void TurnClient::Proxy::setUserPass(const QString &user, const QString &pass)
{
	v_user = user;
	v_pass = pass;
}

//----------------------------------------------------------------------------
// TurnClient
//----------------------------------------------------------------------------
class TurnClient::Private : public QObject
{
	Q_OBJECT

public:
	TurnClient *q;
	Proxy proxy;
	QString clientSoftware;
	TurnClient::Mode mode;
	QString host;
	int port;
	ObjectSession sess;
	ByteStream *bs;
	QCA::TLS *tls;
	bool tlsHandshaken;
	QByteArray inStream;
	StunTransactionPool *pool;
	StunAllocate *allocate;
	bool allocateStarted;
	QString user;
	QCA::SecureArray pass;
	QString realm;
	int retryCount;
	QString errorString;

	class WriteItem
	{
	public:
		enum Type
		{
			Data,
			Other
		};

		Type type;
		int size;
		QHostAddress addr;
		int port;

		WriteItem(int _size) :
			type(Other),
			size(_size),
			port(-1)
		{
		}

		WriteItem(int _size, const QHostAddress &_addr, int _port) :
			type(Data),
			size(_size),
			addr(_addr),
			port(_port)
		{
		}
	};

	QList<WriteItem> writeItems;
	int writtenBytes;
	bool stopping;

	class Packet
	{
	public:
		QHostAddress addr;
		int port;
		QByteArray data;

		// for outbound
		bool requireChannel;

		Packet() :
			port(-1),
			requireChannel(false)
		{
		}
	};

	QList<Packet> in;
	QList<Packet> outPendingPerms;
	int outPendingWrite;
	QList<QHostAddress> desiredPerms;
	QList<StunAllocate::Channel> desiredChannels;

	class Written
	{
	public:
		QHostAddress addr;
		int port;
		int count;
	};

	Private(TurnClient *_q) :
		QObject(_q),
		q(_q),
		sess(this),
		bs(0),
		tls(0),
		pool(0),
		allocate(0),
		retryCount(0),
		writtenBytes(0),
		stopping(false),
		outPendingWrite(0)
	{
	}

	void cleanup()
	{
		delete allocate;
		allocate = 0;
		delete pool;
		pool = 0;
		delete bs;
		bs = 0;

		sess.reset();

		inStream.clear();
		retryCount = 0;
		writeItems.clear();
		writtenBytes = 0;
		stopping = false;
		outPendingPerms.clear();
		outPendingWrite = 0;
	}

	void do_connect()
	{
		if(proxy.type() == Proxy::HttpConnect)
		{
			HttpConnect *s = new HttpConnect(this);
			bs = s;
			connect(s, SIGNAL(connected()), SLOT(bs_connected()));
			connect(s, SIGNAL(error(int)), SLOT(bs_error(int)));
			if(!proxy.user().isEmpty())
				s->setAuth(proxy.user(), proxy.pass());
			s->connectToHost(proxy.host(), proxy.port(), host, port);
		}
		else if(proxy.type() == Proxy::Socks)
		{
			SocksClient *s = new SocksClient(this);
			bs = s;
			connect(s, SIGNAL(connected()), SLOT(bs_connected()));
			connect(s, SIGNAL(error(int)), SLOT(bs_error(int)));
			if(!proxy.user().isEmpty())
				s->setAuth(proxy.user(), proxy.pass());
			s->connectToHost(proxy.host(), proxy.port(), host, port);
		}
		else
		{
			BSocket *s = new BSocket(this);
			bs = s;
			connect(s, SIGNAL(connected()), SLOT(bs_connected()));
			connect(s, SIGNAL(error(int)), SLOT(bs_error(int)));
			s->connectToHost(host, port);
		}

		connect(bs, SIGNAL(connectionClosed()), SLOT(bs_connectionClosed()));
		connect(bs, SIGNAL(delayedCloseFinished()), SLOT(bs_delayedCloseFinished()));
		connect(bs, SIGNAL(readyRead()), SLOT(bs_readyRead()));
		connect(bs, SIGNAL(bytesWritten(int)), SLOT(bs_bytesWritten(int)));
	}

	void do_close()
	{
		stopping = true;

		if(allocate && allocateStarted)
		{
			emit q->debugLine("Deallocating...");
			allocate->stop();
		}
		else
		{
			delete allocate;
			allocate = 0;
			delete pool;
			pool = 0;

			do_transport_close();
		}
	}

	void do_transport_close()
	{
		if(tls && tlsHandshaken)
		{
			tls->close();
		}
		else
		{
			delete tls;
			tls = 0;

			do_sock_close();
		}
	}

	void do_sock_close()
	{
		bool waitForSignal = false;
		if(bs->bytesToWrite() > 0)
			waitForSignal = true;

		bs->close();
		if(!waitForSignal)
		{
			cleanup();
			sess.defer(q, "closed");
		}
	}

	void after_connected()
	{
		pool = new StunTransactionPool(StunTransaction::Tcp, this);
		connect(pool, SIGNAL(outgoingMessage(const QByteArray &, const QHostAddress &, int)), SLOT(pool_outgoingMessage(const QByteArray &, const QHostAddress &, int)));
		connect(pool, SIGNAL(needAuthParams()), SLOT(pool_needAuthParams()));

		pool->setLongTermAuthEnabled(true);
		if(!user.isEmpty())
		{
			pool->setUsername(user);
			pool->setPassword(pass);
			if(!realm.isEmpty())
				pool->setRealm(realm);
		}

		allocate = new StunAllocate(pool);
		connect(allocate, SIGNAL(started()), SLOT(allocate_started()));
		connect(allocate, SIGNAL(stopped()), SLOT(allocate_stopped()));
		connect(allocate, SIGNAL(error(XMPP::StunAllocate::Error)), SLOT(allocate_error(XMPP::StunAllocate::Error)));
		connect(allocate, SIGNAL(permissionsChanged()), SLOT(allocate_permissionsChanged()));
		connect(allocate, SIGNAL(channelsChanged()), SLOT(allocate_channelsChanged()));

		allocate->setClientSoftwareNameAndVersion(clientSoftware);

		allocateStarted = false;
		emit q->debugLine("Allocating...");
		allocate->start();
	}

	void processStream(const QByteArray &in)
	{
		inStream += in;

		while(1)
		{
			QByteArray packet;

			// try to extract ChannelData or a STUN message from
			//   the stream
			packet = StunAllocate::readChannelData((const quint8 *)inStream.data(), inStream.size());
			if(packet.isNull())
			{
				packet = StunMessage::readStun((const quint8 *)inStream.data(), inStream.size());
				if(packet.isNull())
					break;
			}

			inStream = inStream.mid(packet.size());
			processDatagram(in);
		}
	}

	void processDatagram(const QByteArray &buf)
	{
		QByteArray data;
		QHostAddress fromAddr;
		int fromPort;

		bool notStun;
		if(!pool->writeIncomingMessage(buf, &notStun))
		{
			if(notStun)
			{
				// not stun?  maybe it is a data packet
				data = allocate->decode(buf, &fromAddr, &fromPort);
				if(!data.isNull())
				{
					emit q->debugLine("Received ChannelData-based data packet");
					processDataPacket(data, fromAddr, fromPort);
					return;
				}
			}
			else
			{
				// packet might be stun not owned by pool.
				//   let's see
				StunMessage message = StunMessage::fromBinary(buf);
				if(!message.isNull())
				{
					data = allocate->decode(message, &fromAddr, &fromPort);

					if(!data.isNull())
					{
						emit q->debugLine("Received STUN-based data packet");
						processDataPacket(data, fromAddr, fromPort);
					}
					else
						emit q->debugLine("Warning: server responded with an unexpected STUN packet, skipping.");

					return;
				}
			}

			emit q->debugLine("Warning: server responded with what doesn't seem to be a STUN or data packet, skipping.");
		}
	}

	void processDataPacket(const QByteArray &buf, const QHostAddress &addr, int port)
	{
		Packet p;
		p.addr = addr;
		p.port = port;
		p.data = buf;
		in += p;

		emit q->readyRead();
	}

	void writeOrQueue(const QByteArray &buf, const QHostAddress &addr, int port)
	{
		Q_ASSERT(allocateStarted);

		StunAllocate::Channel c(addr, port);
		bool writeImmediately = false;
		bool requireChannel = desiredChannels.contains(c);

		QList<QHostAddress> actualPerms = allocate->permissions();
		if(actualPerms.contains(addr))
		{
			if(requireChannel)
			{
				QList<StunAllocate::Channel> actualChannels = allocate->channels();
				if(actualChannels.contains(c))
					writeImmediately = true;
			}
			else
				writeImmediately = true;
		}

		if(writeImmediately)
		{
			write(buf, addr, port);
		}
		else
		{
			Packet p;
			p.addr = addr;
			p.port = port;
			p.data = buf;
			p.requireChannel = requireChannel;
			outPendingPerms += p;

			ensurePermission(addr);
		}
	}

	void tryWriteQueued()
	{
		QList<QHostAddress> actualPerms = allocate->permissions();
		QList<StunAllocate::Channel> actualChannels = allocate->channels();
		for(int n = 0; n < outPendingPerms.count(); ++n)
		{
			const Packet &p = outPendingPerms[n];
			if(actualPerms.contains(p.addr))
			{
				StunAllocate::Channel c(p.addr, p.port);
				if(!p.requireChannel || actualChannels.contains(c))
				{
					Packet po = outPendingPerms[n];
					outPendingPerms.removeAt(n);
					--n; // adjust position

					write(po.data, po.addr, po.port);
				}
			}
		}
	}

	void write(const QByteArray &buf, const QHostAddress &addr, int port)
	{
		QByteArray packet = allocate->encode(buf, addr, port);
		writeItems += WriteItem(packet.size(), addr, port);
		++outPendingWrite;
		if(tls)
			tls->write(packet);
		else
			bs->write(packet);
	}

	void ensurePermission(const QHostAddress &addr)
	{
		if(!desiredPerms.contains(addr))
		{
			emit q->debugLine(QString("Setting permission for peer address %1").arg(addr.toString()));

			desiredPerms += addr;
			allocate->setPermissions(desiredPerms);
		}
	}

	void addChannelPeer(const QHostAddress &addr, int port)
	{
		ensurePermission(addr);

		StunAllocate::Channel c(addr, port);
		if(!desiredChannels.contains(c))
		{
			emit q->debugLine(QString("Setting channel for peer address/port %1;%2").arg(c.address.toString()).arg(c.port));

			desiredChannels += c;
			allocate->setChannels(desiredChannels);
		}
	}

private slots:
	void bs_connected()
	{
		ObjectSessionWatcher watch(&sess);
		emit q->connected();
		if(!watch.isValid())
			return;

		if(mode == TurnClient::TlsMode)
		{
			tls = new QCA::TLS(this);
			connect(tls, SIGNAL(handshaken()), SLOT(tls_handshaken()));
			connect(tls, SIGNAL(readyRead()), SLOT(tls_readyRead()));
			connect(tls, SIGNAL(readyReadOutgoing()), SLOT(tls_readyReadOutgoing()));
			connect(tls, SIGNAL(error()), SLOT(tls_error()));
			tlsHandshaken = false;
			emit q->debugLine("TLS handshaking...");
			tls->startClient();
		}
		else
			after_connected();
	}

	void bs_connectionClosed()
	{
		cleanup();
		errorString = "Server unexpectedly disconnected.";
		emit q->error(TurnClient::ErrorStream);
	}

	void bs_delayedCloseFinished()
	{
		cleanup();
		emit q->closed();
	}

	void bs_readyRead()
	{
		QByteArray buf = bs->read();

		if(tls)
			tls->writeIncoming(buf);
		else
			processStream(buf);
	}

	void bs_bytesWritten(int written)
	{
		if(tls)
		{
			// convertBytesWritten() is unsafe to call unless
			//   the TLS handshake is completed
			if(!tlsHandshaken)
				return;

			written = tls->convertBytesWritten(written);
		}

		writtenBytes += written;

		QList<Written> writtenDests;
		while(!writeItems.isEmpty() && writtenBytes >= writeItems.first().size)
		{
			WriteItem wi = writeItems.takeFirst();
			writtenBytes -= wi.size;

			if(wi.type == WriteItem::Data)
			{
				int at = -1;
				for(int n = 0; n < writtenDests.count(); ++n)
				{
					if(writtenDests[n].addr == wi.addr && writtenDests[n].port == wi.port)
					{
						at = n;
						break;
					}
				}

				if(at != -1)
				{
					++writtenDests[at].count;
				}
				else
				{
					Written wr;
					wr.addr = wi.addr;
					wr.port = wi.port;
					wr.count = 1;
					writtenDests += wr;
				}
			}
		}

		ObjectSessionWatcher watch(&sess);

		foreach(const Written &wr, writtenDests)
		{
			emit q->packetsWritten(wr.count, wr.addr, wr.port);
			if(!watch.isValid())
				return;
		}
	}

	void bs_error(int e)
	{
		TurnClient::Error te;
		if(qobject_cast<HttpConnect*>(bs))
		{
			if(e == HttpConnect::ErrConnectionRefused)
				te = TurnClient::ErrorConnect;
			else if(e == HttpConnect::ErrHostNotFound)
				te = TurnClient::ErrorHostNotFound;
			else if(e == HttpConnect::ErrProxyConnect)
				te = TurnClient::ErrorProxyConnect;
			else if(e == HttpConnect::ErrProxyNeg)
				te = TurnClient::ErrorProxyNeg;
			else if(e == HttpConnect::ErrProxyAuth)
				te = TurnClient::ErrorProxyAuth;
			else
				te = TurnClient::ErrorStream;
		}
		else if(qobject_cast<SocksClient*>(bs))
		{
			if(e == SocksClient::ErrConnectionRefused)
				te = TurnClient::ErrorConnect;
			else if(e == SocksClient::ErrHostNotFound)
				te = TurnClient::ErrorHostNotFound;
			else if(e == SocksClient::ErrProxyConnect)
				te = TurnClient::ErrorProxyConnect;
			else if(e == SocksClient::ErrProxyNeg)
				te = TurnClient::ErrorProxyNeg;
			else if(e == SocksClient::ErrProxyAuth)
				te = TurnClient::ErrorProxyAuth;
			else
				te = TurnClient::ErrorStream;
		}
		else
		{
			if(e == BSocket::ErrConnectionRefused)
				te = TurnClient::ErrorConnect;
			else if(e == BSocket::ErrHostNotFound)
				te = TurnClient::ErrorHostNotFound;
			else
				te = TurnClient::ErrorStream;
		}

		cleanup();
		errorString = "Transport error.";
		emit q->error(te);
	}

	void tls_handshaken()
	{
		tlsHandshaken = true;

		ObjectSessionWatcher watch(&sess);
		emit q->tlsHandshaken();
		if(!watch.isValid())
			return;

		tls->continueAfterStep();
		after_connected();
	}

	void tls_readyRead()
	{
		processStream(tls->read());
	}

	void tls_readyReadOutgoing()
	{
		bs->write(tls->readOutgoing());
	}

	void tls_closed()
	{
		delete tls;
		tls = 0;

		do_sock_close();
	}

	void tls_error()
	{
		cleanup();
		errorString = "TLS error.";
		emit q->error(TurnClient::ErrorTls);
	}

	void pool_outgoingMessage(const QByteArray &packet, const QHostAddress &toAddress, int toPort)
	{
		// we aren't using IP-associated transactions
		Q_UNUSED(toAddress);
		Q_UNUSED(toPort);

		writeItems += WriteItem(packet.size());

		if(tls)
			tls->write(packet);
		else
			bs->write(packet);
	}

	void pool_needAuthParams()
	{
		emit q->needAuthParams();
	}

	void allocate_started()
	{
		allocateStarted = true;
		emit q->debugLine("Allocate started");

		emit q->activated();
	}

	void allocate_stopped()
	{
		delete allocate;
		allocate = 0;
		delete pool;
		pool = 0;

		do_transport_close();
	}

	void allocate_error(XMPP::StunAllocate::Error e)
	{
		QString str = allocate->errorString();

		TurnClient::Error te;
		if(e == StunAllocate::ErrorAuth)
			te = TurnClient::ErrorAuth;
		else if(e == StunAllocate::ErrorRejected)
			te = TurnClient::ErrorRejected;
		else if(e == StunAllocate::ErrorProtocol)
			te = TurnClient::ErrorProtocol;
		else if(e == StunAllocate::ErrorCapacity)
			te = TurnClient::ErrorCapacity;
		else if(e == StunAllocate::ErrorMismatch)
		{
			++retryCount;
			if(retryCount < 3 && !stopping)
			{
				// start completely over, but don't forget
				//   the retryCount
				int tmp = retryCount;
				cleanup();
				retryCount = tmp;
				do_connect();
				return;
			}
			else
				te = TurnClient::ErrorMismatch;
		}

		cleanup();
		errorString = str;
		emit q->error(te);
	}

	void allocate_permissionsChanged()
	{
		emit q->debugLine("PermissionsChanged");

		tryWriteQueued();
	}

	void allocate_channelsChanged()
	{
		emit q->debugLine("ChannelsChanged");

		tryWriteQueued();
	}
};

TurnClient::TurnClient(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

TurnClient::~TurnClient()
{
	delete d;
}

void TurnClient::setProxy(const Proxy &proxy)
{
	d->proxy = proxy;
}

void TurnClient::setClientSoftwareNameAndVersion(const QString &str)
{
	d->clientSoftware = str;
}

void TurnClient::connectToHost(const QString &host, int port, Mode mode)
{
	d->host = host;
	d->port = port;
	d->mode = mode;
	d->in.clear();
	d->do_connect();
}

QString TurnClient::realm() const
{
	if(d->pool)
		return d->pool->realm();
	else
		return d->realm;
}

void TurnClient::setUsername(const QString &username)
{
	d->user = username;
	if(d->pool)
		d->pool->setUsername(d->user);
}

void TurnClient::setPassword(const QCA::SecureArray &password)
{
	d->pass = password;
	if(d->pool)
		d->pool->setPassword(d->pass);
}

void TurnClient::setRealm(const QString &realm)
{
	d->realm = realm;
	if(d->pool)
		d->pool->setRealm(d->realm);
}

void TurnClient::continueAfterParams()
{
	Q_ASSERT(d->pool);
	d->pool->continueAfterParams();
}

void TurnClient::close()
{
	d->do_close();
}

StunAllocate *TurnClient::stunAllocate()
{
	return d->allocate;
}

void TurnClient::addChannelPeer(const QHostAddress &addr, int port)
{
	d->addChannelPeer(addr, port);
}

int TurnClient::packetsToRead() const
{
	return d->in.count();
}

int TurnClient::packetsToWrite() const
{
	return d->outPendingPerms.count() + d->outPendingWrite;
}

QByteArray TurnClient::read(QHostAddress *addr, int *port)
{
	if(!d->in.isEmpty())
	{
		Private::Packet p = d->in.takeFirst();
		*addr = p.addr;
		*port = p.port;
		return p.data;
	}
	else
		return QByteArray();
}

void TurnClient::write(const QByteArray &buf, const QHostAddress &addr, int port)
{
	d->writeOrQueue(buf, addr, port);
}

QString TurnClient::errorString() const
{
	return d->errorString;
}

}

#include "turnclient.moc"
