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
	QByteArray inStream; // incoming stream
	StunTransactionPool *pool;
	StunAllocate *allocate;
	QString user;
	QCA::SecureArray pass;
	QString realm;

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

		WriteItem(Type _type, int _size)
		{
			type = _type;
			size = _size;
		}
	};

	QList<WriteItem> writeItems;
	int writtenBytes;

	Private(TurnClient *_q) :
		QObject(_q),
		q(_q),
		sess(this),
		bs(0),
		tls(0),
		pool(0),
		allocate(0)
	{
	}

	void cleanup()
	{
		// TODO
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
		if(tls && tlsHandshaken)
			tls->close();
		else
			do_sock_close();
	}

	void do_sock_close()
	{
		bool waitForSignal = false;
		if(bs->bytesToWrite() > 0)
			waitForSignal = true;

		bs->close();
		if(!waitForSignal)
			sess.defer(q, "closed");
	}

	void after_connected()
	{
		pool = new StunTransactionPool(StunTransaction::Tcp, this);
		connect(pool, SIGNAL(outgoingMessage(const QByteArray &, const QHostAddress &, int)), SLOT(pool_outgoingMessage(const QByteArray &, const QHostAddress &, int)));
		connect(pool, SIGNAL(needAuthParams()), SLOT(pool_needAuthParams()));

		if(!user.isEmpty())
		{
			pool->setUsername(user);
			pool->setPassword(pass);
		}

		if(!realm.isEmpty())
			pool->setRealm(realm);

		allocate = new StunAllocate(pool);
		connect(allocate, SIGNAL(started()), SLOT(allocate_started()));
		connect(allocate, SIGNAL(stopped()), SLOT(allocate_stopped()));
		connect(allocate, SIGNAL(error(XMPP::StunAllocate::Error)), SLOT(allocate_error(XMPP::StunAllocate::Error)));
		connect(allocate, SIGNAL(permissionsChanged()), SLOT(allocate_permissionsChanged()));
		connect(allocate, SIGNAL(channelsChanged()), SLOT(allocate_channelsChanged()));

		allocate->setClientSoftwareNameAndVersion(clientSoftware);
		allocate->start();
	}

	void processStream(const QByteArray &in)
	{
		// TODO
		Q_UNUSED(in);
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
			tls->startClient();
		}
		else
			after_connected();
	}

	void bs_connectionClosed()
	{
		cleanup();
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
			written = tls->convertBytesWritten(written);

		int packets = 0;
		writtenBytes += written;
		while(!writeItems.isEmpty() && writtenBytes >= writeItems.first().size)
		{
			WriteItem wi = writeItems.takeFirst();
			writtenBytes -= wi.size;

			if(wi.type == WriteItem::Data)
				++packets;
		}

		if(packets > 0)
			emit q->packetsWritten(packets);
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
		emit q->error(te);
	}

	void tls_handshaken()
	{
		tlsHandshaken = true;
		tls->continueAfterStep();

		ObjectSessionWatcher watch(&sess);
		emit q->tlsHandshaken();
		if(!watch.isValid())
			return;

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
		emit q->error(TurnClient::ErrorTls);
	}

	void pool_outgoingMessage(const QByteArray &packet, const QHostAddress &toAddress, int toPort)
	{
		// we aren't using IP-associated transactions
		Q_UNUSED(toAddress);
		Q_UNUSED(toPort);

		writeItems += WriteItem(WriteItem::Other, packet.size());

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
		// TODO
	}

	void allocate_stopped()
	{
		// TODO
	}

	void allocate_error(XMPP::StunAllocate::Error e)
	{
		// TODO
		Q_UNUSED(e);
	}

	void allocate_permissionsChanged()
	{
		// TODO
	}

	void allocate_channelsChanged()
	{
		// TODO
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

int TurnClient::packetsToRead() const
{
	// TODO
	return 0;
}

int TurnClient::packetsToWrite() const
{
	// TODO
	return 0;
}

QByteArray TurnClient::read(QHostAddress *addr, int *port)
{
	// TODO
	Q_UNUSED(addr);
	Q_UNUSED(port);
	return QByteArray();
}

void TurnClient::write(const QByteArray &buf, const QHostAddress &addr, int port)
{
	// TODO
	Q_UNUSED(buf);
	Q_UNUSED(addr);
	Q_UNUSED(port);
}

}

#include "turnclient.moc"
