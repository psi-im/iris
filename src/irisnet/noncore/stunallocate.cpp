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
#include <QtCrypto>
#include "stunmessage.h"
#include "stuntypes.h"
#include "stuntransaction.h"

Q_DECLARE_METATYPE(XMPP::StunAllocate::Error)

namespace XMPP {

class StunAllocate::Private : public QObject
{
	Q_OBJECT

public:
	StunAllocate *q;
	StunTransactionPool *pool;
	StunTransaction *trans;
	QString clientSoftware, serverSoftware;
	QHostAddress reflexiveAddress, relayedAddress;
	int reflexivePort, relayedPort;
	StunMessage msg;
	QList<QHostAddress> perms;

	Private(StunAllocate *_q) :
		QObject(_q),
		q(_q),
		pool(0),
		trans(0)
	{
		qRegisterMetaType<StunAllocate::Error>();
	}

	void start()
	{
		Q_ASSERT(!trans);

		trans = new StunTransaction(this);
		connect(trans, SIGNAL(createMessage(const QByteArray &)), SLOT(trans_createMessage(const QByteArray &)));
		connect(trans, SIGNAL(finished(const XMPP::StunMessage &)), SLOT(trans_finished(const XMPP::StunMessage &)));
		connect(trans, SIGNAL(error(XMPP::StunTransaction::Error)), SLOT(trans_error(XMPP::StunTransaction::Error)));

		trans->start(pool);
	}

	void stop()
	{
		// TODO
		QMetaObject::invokeMethod(q, "stopped", Qt::QueuedConnection);
	}

	void setPermissions(const QList<QHostAddress> &newPerms)
	{
		perms = newPerms;

		trans = new StunTransaction(this);
		connect(trans, SIGNAL(createMessage(const QByteArray &)), SLOT(trans_createMessage(const QByteArray &)));
		connect(trans, SIGNAL(finished(const XMPP::StunMessage &)), SLOT(trans_finished(const XMPP::StunMessage &)));
		connect(trans, SIGNAL(error(XMPP::StunTransaction::Error)), SLOT(trans_error(XMPP::StunTransaction::Error)));

		trans->start(pool);
	}

private slots:
	void trans_createMessage(const QByteArray &transactionId)
	{
		if(perms.isEmpty())
		{
			StunMessage message;
			message.setMethod(StunTypes::Allocate);
			message.setId((const quint8 *)transactionId.data());

			QList<StunMessage::Attribute> list;

			if(!clientSoftware.isEmpty())
			{
				StunMessage::Attribute a;
				a.type = StunTypes::SOFTWARE;
				a.value = StunTypes::createSoftware(clientSoftware);
				list += a;
			}

			{
				StunMessage::Attribute a;
				a.type = StunTypes::LIFETIME;
				a.value = StunTypes::createLifetime(3600);
				list += a;
			}

			{
				StunMessage::Attribute a;
				a.type = StunTypes::REQUESTED_TRANSPORT;
				a.value = StunTypes::createRequestedTransport(17); // 17=UDP
				list += a;
			}

			{
				StunMessage::Attribute a;
				a.type = StunTypes::DONT_FRAGMENT;
				list += a;
			}

			message.setAttributes(list);

			trans->setMessage(message);
		}
		else
		{
			StunMessage message;
			message.setMethod(StunTypes::CreatePermission);
			message.setId((const quint8 *)transactionId.data());

			QList<StunMessage::Attribute> list;

			{
				StunMessage::Attribute a;
				a.type = StunTypes::XOR_PEER_ADDRESS;
				a.value = StunTypes::createXorPeerAddress(perms.first(), 0, message.magic(), message.id());
				list += a;
			}

			message.setAttributes(list);

			trans->setMessage(message);
		}
	}

	void trans_finished(const XMPP::StunMessage &response)
	{
		//printf("trans_finished\n");
		delete trans;
		trans = 0;

		// HACK
		if(perms.isEmpty())
		{
			QHostAddress addr;
			quint16 port;

			if(StunTypes::parseXorRelayedAddress(response.attribute(StunTypes::XOR_MAPPED_ADDRESS), response.magic(), response.id(), &addr, &port))
			{
				relayedAddress = addr;
				relayedPort = port;
			}

			if(StunTypes::parseXorMappedAddress(response.attribute(StunTypes::XOR_MAPPED_ADDRESS), response.magic(), response.id(), &addr, &port))
			{
				reflexiveAddress = addr;
				reflexivePort = port;
			}

			emit q->started();
		}
		else
			emit q->permissionsChanged();

#if 0
		StunTypes::print_packet(response);

		pool->remove(trans);
		trans = 0;

		if(response.mclass() == StunMessage::ErrorResponse)
		{
			//errorString = "Server responded with an error.";
			//emit q->error(StunBinding::ErrorRejected);

			if(!msg.attribute(StunTypes::NONCE).isNull())
				return;

			QList<StunMessage::Attribute> list = msg.attributes();

			{
				StunMessage::Attribute a;
				a.type = StunTypes::REALM;
				a.value = "domain.org"; //response.attribute(StunTypes::REALM);
				list += a;
			}

			{
				StunMessage::Attribute a;
				a.type = StunTypes::NONCE;
				a.value = response.attribute(StunTypes::NONCE);
				list += a;
			}

			msg.setAttributes(list);

			trans = new StunTransaction(this);
			connect(trans, SIGNAL(finished(const XMPP::StunMessage &)), SLOT(trans_finished(const XMPP::StunMessage &)));
			connect(trans, SIGNAL(error(XMPP::StunTransaction::Error)), SLOT(trans_error(XMPP::StunTransaction::Error)));
			trans->start(pool->mode(), msg, "toto", "toto:domain.org:password");

			pool->insert(trans);

			return;
		}

		/*QHostAddress saddr;
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
		emit q->success();*/
#endif
	}

	void trans_error(XMPP::StunTransaction::Error e)
	{
		printf("trans_error\n");
		/*pool->remove(trans);
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
		}*/
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

void StunAllocate::setClientSoftwareNameAndVersion(const QString &str)
{
	d->clientSoftware = str;
}

void StunAllocate::start()
{
	d->start();
}

void StunAllocate::stop()
{
	d->stop();
}

QString StunAllocate::serverSoftwareNameAndVersion() const
{
	return d->serverSoftware;
}

QHostAddress StunAllocate::reflexiveAddress() const
{
	return d->reflexiveAddress;
}

int StunAllocate::reflexivePort() const
{
	return d->reflexivePort;
}

QHostAddress StunAllocate::relayedAddress() const
{
	return d->relayedAddress;
}

int StunAllocate::relayedPort() const
{
	return d->relayedPort;
}

QList<QHostAddress> StunAllocate::permissions() const
{
	return d->perms;
}

void StunAllocate::setPermissions(const QList<QHostAddress> &perms)
{
	d->setPermissions(perms);
}

int StunAllocate::packetHeaderOverhead(const QHostAddress &addr) const
{
	// TODO
	Q_UNUSED(addr);
	return 0;
}

QByteArray StunAllocate::encode(const QByteArray &datagram, const QHostAddress &addr, int port)
{
	StunMessage message;
	message.setClass(StunMessage::Indication);
	message.setMethod(StunTypes::Send);
	QByteArray id = d->pool->generateId();
	message.setId((const quint8 *)id.data());

	QList<StunMessage::Attribute> list;

	{
		StunMessage::Attribute a;
		a.type = StunTypes::XOR_PEER_ADDRESS;
		a.value = StunTypes::createXorPeerAddress(addr, port, message.magic(), message.id());
		list += a;
	}

	{
		StunMessage::Attribute a;
		a.type = StunTypes::DATA;
		a.value = datagram;
		list += a;
	}

	message.setAttributes(list);

	QByteArray out = message.toBinary();
	//StunMessage tmp = StunMessage::fromBinary(out);
	//StunTypes::print_packet(message);
	//StunTypes::print_packet(tmp);

	return out; //return message.toBinary();
}

QByteArray StunAllocate::decode(const QByteArray &encoded, QHostAddress *addr, int *port)
{
	printf("decode raw\n");
	// TODO
	Q_UNUSED(encoded);
	Q_UNUSED(addr);
	Q_UNUSED(port);
	return QByteArray();
}

QByteArray StunAllocate::decode(const StunMessage &encoded, QHostAddress *addr, int *port)
{
	QHostAddress paddr;
	quint16 pport;

	if(!StunTypes::parseXorPeerAddress(encoded.attribute(StunTypes::XOR_PEER_ADDRESS), encoded.magic(), encoded.id(), &paddr, &pport))
		return QByteArray();

	QByteArray data = encoded.attribute(StunTypes::DATA);
	if(data.isNull())
		return QByteArray();

	*addr = paddr;
	*port = pport;
	return data;
}

QString StunAllocate::errorString() const
{
	// TODO
	return QString();
}

}

#include "stunallocate.moc"
