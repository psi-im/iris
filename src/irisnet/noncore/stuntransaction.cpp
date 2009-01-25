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
#include "stunmessage.h"

Q_DECLARE_METATYPE(XMPP::StunTransaction::Error)

namespace XMPP {

//----------------------------------------------------------------------------
// StunTransaction
//----------------------------------------------------------------------------
class StunTransaction::Private : public QObject
{
	Q_OBJECT

public:
	StunTransaction *q;
	bool active;
	StunTransaction::Mode mode;
	QByteArray id;
	QByteArray packet;
	int rto, rc, rm, ti;
	int tries;
	int last_interval;
	QTimer *t;
	//QTime time;

	Private(StunTransaction *_q) :
		QObject(_q),
		q(_q)
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

	void start(StunTransaction::Mode _mode, const StunMessage &msg)
	{
		mode = _mode;
		StunMessage out = msg;

		id = QCA::Random::randomArray(12).toByteArray();
		out.setId((const quint8 *)id.data());

		packet = out.toBinary();
		if(packet.isEmpty())
		{
			// since a transaction is not cancelable nor reusable,
			//   there's no DOR-SR issue here
			QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection,
				Q_ARG(XMPP::StunTransaction::Error, ErrorGeneric));
			return;
		}

		tries = 1; // assume the user does its job

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

		//time.start();
		//printf("send: %d\n", time.elapsed());
	}

private slots:
	void t_timeout()
	{
		if(mode == StunTransaction::Tcp || tries == rc)
		{
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

		//printf("send: %d\n", time.elapsed());
		emit q->retransmit();
	}

public:
	bool processIncoming(const StunMessage &msg)
	{
		if(msg.mclass() != StunMessage::SuccessResponse && msg.mclass() != StunMessage::ErrorResponse)
			return false;

		if(memcmp(msg.id(), id.data(), 12) != 0)
			return false;

		emit q->finished(msg);
		return true;
	}
};

StunTransaction::StunTransaction(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

StunTransaction::~StunTransaction()
{
	delete d;
}

void StunTransaction::start(Mode mode, const StunMessage &msg)
{
	Q_ASSERT(!d->active);
	d->start(mode, msg);
}

QByteArray StunTransaction::transactionId() const
{
	return d->id;
}

QByteArray StunTransaction::packet() const
{
	return d->packet;
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

bool StunTransaction::writeIncomingMessage(const StunMessage &msg)
{
	return d->processIncoming(msg);
}

//----------------------------------------------------------------------------
// StunTransactionPool
//----------------------------------------------------------------------------
class StunTransactionPool::Private : public QObject
{
	Q_OBJECT

public:
	StunTransactionPool *q;
	QHash<StunTransaction*,QByteArray> transToId;
	QHash<QByteArray,StunTransaction*> idToTrans;

	Private(StunTransactionPool *_q) :
		QObject(_q),
		q(_q)
	{
	}

	void insert(StunTransaction *trans)
	{
		connect(trans, SIGNAL(retransmit()), this, SLOT(trans_retransmit()));
		connect(trans, SIGNAL(finished(const XMPP::StunMessage &)), this, SLOT(trans_finished(const XMPP::StunMessage &)));
		connect(trans, SIGNAL(error(XMPP::StunTransaction::Error)), this, SLOT(trans_error(XMPP::StunTransaction::Error)));

		QByteArray id = trans->transactionId();
		transToId.insert(trans, id);
		idToTrans.insert(id, trans);
	}

	void remove(StunTransaction *trans)
	{
		disconnect(trans, SIGNAL(retransmit()), this, SLOT(trans_retransmit()));
		disconnect(trans, SIGNAL(finished(const XMPP::StunMessage &)), this, SLOT(trans_finished(const XMPP::StunMessage &)));
		disconnect(trans, SIGNAL(error(XMPP::StunTransaction::Error)), this, SLOT(trans_error(XMPP::StunTransaction::Error)));

		QByteArray id = transToId.value(trans);
		transToId.remove(trans);
		idToTrans.remove(id);
	}

private slots:
	void trans_retransmit()
	{
		StunTransaction *trans = (StunTransaction *)sender();
		emit q->retransmit(trans);
	}

	void trans_finished(const XMPP::StunMessage &response)
	{
		StunTransaction *trans = (StunTransaction *)sender();
		remove(trans);
		emit q->finished(trans, response);
	}

	void trans_error(XMPP::StunTransaction::Error error)
	{
		StunTransaction *trans = (StunTransaction *)sender();
		remove(trans);
		emit q->error(trans, error);
	}
};

StunTransactionPool::StunTransactionPool(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

StunTransactionPool::~StunTransactionPool()
{
	delete d;
}

void StunTransactionPool::insert(StunTransaction *trans)
{
	Q_ASSERT(!trans->transactionId().isEmpty());
	d->insert(trans);
}

void StunTransactionPool::remove(StunTransaction *trans)
{
	d->remove(trans);
}

bool StunTransactionPool::writeIncomingMessage(const StunMessage &msg)
{
	if(msg.mclass() != StunMessage::SuccessResponse && msg.mclass() != StunMessage::ErrorResponse)
		return false;

	StunTransaction *trans = d->idToTrans.value(QByteArray::fromRawData((const char *)msg.id(), 12));
	if(!trans)
		return false;

	return trans->writeIncomingMessage(msg);
}

}

#include "stuntransaction.moc"
