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

#include "stunbinding.h"

#include <QHostAddress>
#include "stuntransaction.h"
#include "stunmessage.h"

namespace XMPP {

// pass valid magic and id pointers to do XOR-MAPPED-ADDRESS processing
// pass 0 for magic and id to do MAPPED-ADDRESS processing
static bool parse_mapped_address(const QByteArray &val, const quint8 *magic, const quint8 *id, QHostAddress *addr, quint16 *port)
{
	// val is at least 4 bytes
	if(val.size() < 4)
		return false;

	const quint8 *p = (const quint8 *)val.data();

	if(p[0] != 0)
		return false;

	quint16 _port;
	if(magic)
	{
		_port = p[2] ^ magic[0];
		_port <<= 8;
		_port += p[3] ^ magic[1];
	}
	else
	{
		_port = p[2];
		_port <<= 8;
		_port += p[3];
	}

	QHostAddress _addr;

	if(p[1] == 0x01)
	{
		// ipv4

		// val is 8 bytes in this case
		if(val.size() != 8)
			return false;

		quint32 addr4;
		if(magic)
		{
			addr4 = p[4] ^ magic[0];
			addr4 <<= 8;
			addr4 += p[5] ^ magic[1];
			addr4 <<= 8;
			addr4 += p[6] ^ magic[2];
			addr4 <<= 8;
			addr4 += p[7] ^ magic[3];
		}
		else
		{
			addr4 = p[4];
			addr4 <<= 8;
			addr4 += p[5];
			addr4 <<= 8;
			addr4 += p[6];
			addr4 <<= 8;
			addr4 += p[7];
		}
		_addr = QHostAddress(addr4);
	}
	else if(p[1] == 0x02)
	{
		// ipv6

		// val is 20 bytes in this case
		if(val.size() != 20)
			return false;

		quint8 tmp[16];
		for(int n = 0; n < 16; ++n)
		{
			quint8 x;
			if(n < 4)
				x = magic[n];
			else
				x = id[n - 4];

			tmp[n] = p[n + 4] ^ x;
		}

		_addr = QHostAddress(tmp);
	}
	else
		return false;

	*addr = _addr;
	*port = _port;
	return true;
}

class StunBinding::Private : public QObject
{
	Q_OBJECT

public:
	StunBinding *q;
	StunTransactionPool *pool;
	StunTransaction *trans;
	QHostAddress addr;
	int port;
	QString errorString;

	Private(StunBinding *_q) :
		QObject(_q),
		q(_q),
		pool(0),
		trans(0)
	{
	}

	~Private()
	{
		if(trans)
			pool->remove(trans);
	}

	void start()
	{
		Q_ASSERT(!trans);

		trans = new StunTransaction(this);
		connect(trans, SIGNAL(finished(const XMPP::StunMessage &)), SLOT(trans_finished(const XMPP::StunMessage &)));
		connect(trans, SIGNAL(error(XMPP::StunTransaction::Error)), SLOT(trans_error(XMPP::StunTransaction::Error)));

		StunMessage message;
		message.setClass(StunMessage::Request);
		message.setMethod(0x001);
		QByteArray id = pool->generateId();
		message.setId((const quint8 *)id.data());

		trans->start(pool->mode(), message);

		pool->insert(trans);
	}

private slots:
	void trans_finished(const XMPP::StunMessage &response)
	{
		pool->remove(trans);
		trans = 0;

		if(response.mclass() == StunMessage::ErrorResponse)
		{
			errorString = "Server responded with an error.";
			emit q->error(StunBinding::ErrorRejected);
			return;
		}

		QHostAddress saddr;
		quint16 sport = 0;

		QByteArray val;
		val = response.attribute(0x0020);
		if(!val.isNull())
		{
			if(!parse_mapped_address(val, response.magic(), response.id(), &saddr, &sport))
			{
				errorString = "Unable to parse XOR-MAPPED-ADDRESS response.";
				emit q->error(StunBinding::ErrorProtocol);
				return;
			}
		}
		else
		{
			val = response.attribute(0x0001);
			if(!val.isNull())
			{
				if(!parse_mapped_address(val, 0, 0, &saddr, &sport))
				{
					errorString = "Unable to parse MAPPED-ADDRESS response.";
					emit q->error(StunBinding::ErrorProtocol);
					return;
				}
			}
			else
			{
				errorString = "Response does not contain XOR-MAPPED-ADDRESS or MAPPED-ADDRESS.";
				emit q->error(StunBinding::ErrorProtocol);
				return;
			}
		}

		addr = saddr;
		port = sport;
		emit q->success();
	}

	void trans_error(XMPP::StunTransaction::Error e)
	{
		pool->remove(trans);
		trans = 0;

		if(e == StunTransaction::ErrorTimeout)
		{
			errorString = "Request timed out.";
			emit q->error(StunBinding::ErrorTimeout);
		}
		else
		{
			errorString = "Generic transaction error.";
			emit q->error(StunBinding::ErrorGeneric);
		}
	}
};

StunBinding::StunBinding(StunTransactionPool *pool) :
	QObject(pool)
{
	d = new Private(this);
	d->pool = pool;
}

StunBinding::~StunBinding()
{
	delete d;
}

void StunBinding::start()
{
	d->start();
}

QHostAddress StunBinding::reflexiveAddress() const
{
	return d->addr;
}

int StunBinding::reflexivePort() const
{
	return d->port;
}

QString StunBinding::errorString() const
{
	return d->errorString;
}

}

#include "stunbinding.moc"
