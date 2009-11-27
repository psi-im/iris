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

#include "stuntransaction.h"

#include <QHash>
#include <QMetaType>
#include <QTime>
#include <QTimer>
#include <QtCrypto>
#include "stunutil.h"
#include "stunmessage.h"
#include "stuntypes.h"

//#define STUNTRANSACTION_DEBUG

Q_DECLARE_METATYPE(XMPP::StunTransaction::Error)

namespace XMPP {

class StunTransactionPoolPrivate : public QObject
{
	Q_OBJECT

public:
	StunTransactionPool *q;
	StunTransaction::Mode mode;
	QHash<StunTransaction*,QByteArray> transToId;
	QHash<QByteArray,StunTransaction*> idToTrans;
	QString user;
	QCA::SecureArray pass;
	QString realm;
	QString nonce;

	StunTransactionPoolPrivate(StunTransactionPool *_q) :
		QObject(_q),
		q(_q)
	{
	}

	QByteArray generateId() const;
	void insert(StunTransaction *trans);
	void remove(StunTransaction *trans);
	void transmit(StunTransaction *trans);
};

//----------------------------------------------------------------------------
// StunTransaction
//----------------------------------------------------------------------------
class StunTransactionPrivate : public QObject
{
	Q_OBJECT

public:
	StunTransaction *q;
	StunTransactionPool *pool;
	bool active;
	StunTransaction::Mode mode;
	StunMessage origMessage;
	QByteArray id;
	QByteArray packet;
	int rto, rc, rm, ti;
	int tries;
	int last_interval;
	QTimer *t;
	QString stuser;
	QString stpass;
	QByteArray key;
	QHostAddress to_addr;
	int to_port;
	bool triedLtAuth;
#ifdef STUNTRANSACTION_DEBUG
	QTime time;
#endif

	StunTransactionPrivate(StunTransaction *_q) :
		QObject(_q),
		q(_q),
		pool(0),
		triedLtAuth(false)
	{
		qRegisterMetaType<StunTransaction::Error>();

		active = false;

		t = new QTimer(this);
		connect(t, SIGNAL(timeout()), SLOT(t_timeout()));
		t->setSingleShot(true);

		// defaults from RFC 5389
		rto = 500;
		rc = 7;
		rm = 16;
		ti = 39500;
	}

	~StunTransactionPrivate()
	{
		if(pool)
			pool->d->remove(q);

		t->disconnect(this);
		t->setParent(0);
		t->deleteLater();
	}

	void start(StunTransactionPool *_pool, const QHostAddress &toAddress, int toPort)
	{
		pool = _pool;
		mode = pool->d->mode;
		to_addr = toAddress;
		to_port = toPort;

		tryRequest();
	}

	void setMessage(const StunMessage &request)
	{
		origMessage = request;
	}

	void retry()
	{
		Q_ASSERT(!active);
		pool->d->remove(q);

		tryRequest();
	}

	void tryRequest()
	{
		emit q->createMessage(pool->d->generateId());

		if(origMessage.isNull())
		{
			// since a transaction is not cancelable nor reusable,
			//   there's no DOR-SR issue here
			QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection,
				Q_ARG(XMPP::StunTransaction::Error, StunTransaction::ErrorGeneric));
			return;
		}

		StunMessage out = origMessage;

		out.setClass(StunMessage::Request);
		id = QByteArray((const char *)out.id(), 12);

		if(!stuser.isEmpty())
		{
			QList<StunMessage::Attribute> list = out.attributes();
			StunMessage::Attribute attr;
			attr.type = StunTypes::USERNAME;
			attr.value = StunTypes::createUsername(QString::fromUtf8(StunUtil::saslPrep(stuser.toUtf8()).toByteArray()));
			list += attr;
			out.setAttributes(list);

			key = StunUtil::saslPrep(stpass.toUtf8()).toByteArray();
		}
		else if(!pool->d->nonce.isEmpty())
		{
			QList<StunMessage::Attribute> list = out.attributes();
			{
				StunMessage::Attribute attr;
				attr.type = StunTypes::USERNAME;
				attr.value = StunTypes::createUsername(QString::fromUtf8(StunUtil::saslPrep(pool->d->user.toUtf8()).toByteArray()));
				list += attr;
			}
			{
				StunMessage::Attribute attr;
				attr.type = StunTypes::REALM;
				attr.value = StunTypes::createRealm(pool->d->realm);
				list += attr;
			}
			{
				StunMessage::Attribute attr;
				attr.type = StunTypes::NONCE;
				attr.value = StunTypes::createNonce(pool->d->nonce);
				list += attr;
			}
			out.setAttributes(list);

			QCA::SecureArray buf;
			buf += StunUtil::saslPrep(pool->d->user.toUtf8());
			buf += QByteArray(1, ':');
			buf += StunUtil::saslPrep(pool->d->realm.toUtf8());
			buf += QByteArray(1, ':');
			buf += StunUtil::saslPrep(pool->d->pass);

			key = QCA::Hash("md5").process(buf).toByteArray();
		}

		if(!key.isEmpty())
			packet = out.toBinary(StunMessage::MessageIntegrity | StunMessage::Fingerprint, key);
		else
			packet = out.toBinary();

		if(packet.isEmpty())
		{
			// since a transaction is not cancelable nor reusable,
			//   there's no DOR-SR issue here
			QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection,
				Q_ARG(XMPP::StunTransaction::Error, StunTransaction::ErrorGeneric));
			return;
		}

		active = true;
		tries = 1; // we transmit immediately here, so count it

		if(mode == StunTransaction::Udp)
		{
			last_interval = rm * rto;
			t->start(rto);
			rto *= 2;
		}
		else if(mode == StunTransaction::Tcp)
		{
			t->start(ti);
		}
		else
			Q_ASSERT(0);

#ifdef STUNTRANSACTION_DEBUG
		time.start();
#endif
		pool->d->insert(q);
		transmit();
	}

private slots:
	void t_timeout()
	{
		if(mode == StunTransaction::Tcp || tries == rc)
		{
			pool->d->remove(q);
			emit q->error(StunTransaction::ErrorTimeout);
			return;
		}

		++tries;
		if(tries == rc)
		{
			t->start(last_interval);
		}
		else
		{
			t->start(rto);
			rto *= 2;
		}

		transmit();
	}

private:
	void transmit()
	{
#ifdef STUNTRANSACTION_DEBUG
		printf("STUN SEND: elapsed=%d", time.elapsed());
		if(!to_addr.isNull())
			printf(" to=(%s;%d)", qPrintable(to_addr.toString()), to_port);
		printf("\n");
		StunMessage msg = StunMessage::fromBinary(packet);
		StunTypes::print_packet(msg);
#endif
		pool->d->transmit(q);
	}

public:
	bool writeIncomingMessage(const StunMessage &msg, const QHostAddress &from_addr, int from_port)
	{
		// if a StunMessage is passed directly to us then we assume
		//   the user has done his own integrity checks, if any.
		int validationFlags = StunMessage::MessageIntegrity | StunMessage::Fingerprint;

		return processIncoming(msg, validationFlags, from_addr, from_port);
	}

	bool writeIncomingMessage(const QByteArray &packet, const QHostAddress &from_addr, int from_port)
	{
		int validationFlags = 0;

		// NOTE: this code is kind of goofy but hopefully never results
		//   in a packet being fully parsed twice.  the integrity
		//   checks performed by fromBinary are minimal.  it seems like
		//   this code belongs in StunMessage though.
		StunMessage::ConvertResult result;
		StunMessage msg = StunMessage::fromBinary(packet, &result, StunMessage::MessageIntegrity | StunMessage::Fingerprint, key);
		if(result == StunMessage::ConvertGood)
		{
			validationFlags = StunMessage::MessageIntegrity | StunMessage::Fingerprint;
		}
		else
		{
			msg = StunMessage::fromBinary(packet, &result, StunMessage::MessageIntegrity, key);
			if(result == StunMessage::ConvertGood)
			{
				validationFlags = StunMessage::MessageIntegrity;
			}
			else
			{
				msg = StunMessage::fromBinary(packet, &result, StunMessage::Fingerprint);
				if(result == StunMessage::ConvertGood)
				{
					validationFlags = StunMessage::Fingerprint;
				}
				else
				{
					msg = StunMessage::fromBinary(packet, &result);
					if(result != StunMessage::ConvertGood)
						return false;
				}
			}
		}

		return processIncoming(msg, validationFlags, from_addr, from_port);
	}

	bool processIncoming(const StunMessage &msg, int validationFlags, const QHostAddress &from_addr, int from_port)
	{
		if(!active)
			return false;

		//if(memcmp(msg.id(), id.data(), 12) != 0)
		//	return false;

		if(!to_addr.isNull() && (to_addr != from_addr || to_port != from_port))
			return false;

		active = false;
		t->stop();

#ifdef STUNTRANSACTION_DEBUG
		printf("matched incoming response to existing request.  elapsed=%d\n", time.elapsed());
#endif

		// we'll handle certain error codes at this layer
		int code;
		QString reason;
		if(StunTypes::parseErrorCode(msg.attribute(StunTypes::ERROR_CODE), &code, &reason))
		{
			if(code == StunTypes::Unauthorized && !triedLtAuth)
			{
				QString realm;
				QString nonce;
				if(StunTypes::parseRealm(msg.attribute(StunTypes::REALM), &realm) &&
					StunTypes::parseRealm(msg.attribute(StunTypes::NONCE), &nonce))
				{
					if(pool->d->realm.isEmpty())
						pool->d->realm = realm;
					pool->d->nonce = nonce;

					emit pool->needAuthParams();
					return true;
				}
			}
			else if(code == StunTypes::StaleNonce)
			{
				QString nonce;
				if(StunTypes::parseNonce(msg.attribute(StunTypes::NONCE), &nonce) && nonce != pool->d->nonce)
				{
					pool->d->nonce = nonce;
					retry();
					return true;
				}
			}
		}

		// require message integrity when auth is used
		if((!stuser.isEmpty() || !pool->d->nonce.isEmpty()) && !(validationFlags & StunMessage::MessageIntegrity))
			return false;

		// TODO: care about fingerprint?

		emit q->finished(msg);
		return true;
	}

public slots:
	void continueAfterParams()
	{
		triedLtAuth = true;
		retry();
	}
};

StunTransaction::StunTransaction(QObject *parent) :
	QObject(parent)
{
	d = new StunTransactionPrivate(this);
}

StunTransaction::~StunTransaction()
{
	delete d;
}

void StunTransaction::start(StunTransactionPool *pool, const QHostAddress &toAddress, int toPort)
{
	Q_ASSERT(!d->active);
	d->start(pool, toAddress, toPort);
}

void StunTransaction::setMessage(const StunMessage &request)
{
	d->setMessage(request);
}

void StunTransaction::setRTO(int i)
{
	Q_ASSERT(!d->active);
	d->rto = i;
}

void StunTransaction::setRc(int i)
{
	Q_ASSERT(!d->active);
	d->rc = i;
}

void StunTransaction::setRm(int i)
{
	Q_ASSERT(!d->active);
	d->rm = i;
}

void StunTransaction::setTi(int i)
{
	Q_ASSERT(!d->active);
	d->ti = i;
}

void StunTransaction::setShortTermUsername(const QString &username)
{
	d->stuser = username;
}

void StunTransaction::setShortTermPassword(const QString &password)
{
	d->stpass = password;
}

//----------------------------------------------------------------------------
// StunTransactionPool
//----------------------------------------------------------------------------
QByteArray StunTransactionPoolPrivate::generateId() const
{
	QByteArray id;

	do
	{
		id = QCA::Random::randomArray(12).toByteArray();
	} while(idToTrans.contains(id));

	return id;
}

void StunTransactionPoolPrivate::insert(StunTransaction *trans)
{
	Q_ASSERT(!trans->d->id.isEmpty());
	QByteArray id = trans->d->id;
	transToId.insert(trans, id);
	idToTrans.insert(id, trans);
}

void StunTransactionPoolPrivate::remove(StunTransaction *trans)
{
	QByteArray id = transToId.value(trans);
	transToId.remove(trans);
	idToTrans.remove(id);
}

void StunTransactionPoolPrivate::transmit(StunTransaction *trans)
{
	emit q->outgoingMessage(trans->d->packet, trans->d->to_addr, trans->d->to_port);
}

StunTransactionPool::StunTransactionPool(StunTransaction::Mode mode, QObject *parent) :
	QObject(parent)
{
	d = new StunTransactionPoolPrivate(this);
	d->mode = mode;
}

StunTransactionPool::~StunTransactionPool()
{
	delete d;
}

StunTransaction::Mode StunTransactionPool::mode() const
{
	return d->mode;
}

bool StunTransactionPool::writeIncomingMessage(const StunMessage &msg, const QHostAddress &addr, int port)
{
#ifdef STUNTRANSACTION_DEBUG
	printf("STUN RECV");
	if(!addr.isNull())
		printf(" from=(%s;%d)", qPrintable(addr.toString()), port);
	printf("\n");
	StunTypes::print_packet(msg);
#endif

	QByteArray id = QByteArray::fromRawData((const char *)msg.id(), 12);
	StunMessage::Class mclass = msg.mclass();

	if(mclass != StunMessage::SuccessResponse && mclass != StunMessage::ErrorResponse)
		return false;

	StunTransaction *trans = d->idToTrans.value(id);
	if(!trans)
		return false;

	return trans->d->writeIncomingMessage(msg, addr, port);
}

bool StunTransactionPool::writeIncomingMessage(const QByteArray &packet, const QHostAddress &addr, int port)
{
	if(!StunMessage::isProbablyStun(packet))
		return false;

#ifdef STUNTRANSACTION_DEBUG
	StunMessage msg = StunMessage::fromBinary(packet);
	printf("STUN RECV");
	if(!addr.isNull())
		printf(" from=(%s;%d)", qPrintable(addr.toString()), port);
	printf("\n");
	StunTypes::print_packet(msg);
#endif

	// isProbablyStun ensures the packet is 20 bytes long, so we can safely
	//   safely extract out the transaction id from the raw packet
	QByteArray id = QByteArray((const char *)packet.data() + 8, 12);

	StunMessage::Class mclass = StunMessage::extractClass(packet);

	if(mclass != StunMessage::SuccessResponse && mclass != StunMessage::ErrorResponse)
		return false;

	StunTransaction *trans = d->idToTrans.value(id);
	if(!trans)
		return false;

	return trans->d->writeIncomingMessage(packet, addr, port);
}

void StunTransactionPool::setLongTermAuthEnabled(bool enabled)
{
	Q_UNUSED(enabled);
	// TODO
}

QString StunTransactionPool::realm() const
{
	return d->realm;
}

void StunTransactionPool::setUsername(const QString &username)
{
	d->user = username;
}

void StunTransactionPool::setPassword(const QCA::SecureArray &password)
{
	d->pass = password;
}

void StunTransactionPool::setRealm(const QString &realm)
{
	d->realm = realm;
}

void StunTransactionPool::continueAfterParams()
{
#ifdef STUNTRANSACTION_DEBUG
	printf("continue after params:\n  U=[%s]\n  P=[%s]\n  R=[%s]\n  N=[%s]\n", qPrintable(d->user), d->pass.data(), qPrintable(d->realm), qPrintable(d->nonce));
#endif

	// collect a list of inactive stun transactions that need to do auth
	QList<StunTransaction*> list;
	QHashIterator<StunTransaction*,QByteArray> it(d->transToId);
	while(it.hasNext())
	{
		it.next();
		StunTransaction *trans = it.key();
		if(!trans->d->active && !trans->d->triedLtAuth)
			list += trans;
	}

	foreach(StunTransaction *trans, list)
	{
		QMetaObject::invokeMethod(trans->d, "continueAfterParams",
			Qt::QueuedConnection);
	}
}

QByteArray StunTransactionPool::generateId() const
{
	return d->generateId();
}

}

#include "stuntransaction.moc"
