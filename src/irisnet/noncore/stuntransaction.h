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

#ifndef STUNTRANSACTION_H
#define STUNTRANSACTION_H

#include <QObject>
#include <QByteArray>

namespace XMPP {

class StunMessage;

class StunTransaction : public QObject
{
	Q_OBJECT

public:
	enum Mode
	{
		Udp, // handle retransmissions
		Tcp  // send once
	};

	enum Error
	{
		ErrorGeneric,
		ErrorTimeout
	};

	StunTransaction(QObject *parent = 0);
	~StunTransaction();

	// pass a message with transaction id unset.  it will be filled in.
	//   after calling this function, immediately obtain the result by
	//   calling packet(), and send it.  the start() function will not
	//   perform the first send attempt.  it leaves that to you.
	void start(Mode mode, const StunMessage &request);

	QByteArray transactionId() const;
	QByteArray packet() const;

	// transmission/timeout parameters, from RFC 5389.  by default,
	//   they are set to the recommended values from the RFC.
	void setRTO(int i);
	void setRc(int i);
	void setRm(int i);
	void setTi(int i);

	// note: not DOR-DS safe.  this will either emit signals and return
	//   true, or not emit signals and return false.
	bool writeIncomingMessage(const StunMessage &msg);

signals:
	// indicates you should retransmit the value of packet()
	void retransmit();
	void finished(const XMPP::StunMessage &response);
	void error(XMPP::StunTransaction::Error error);

private:
	Q_DISABLE_COPY(StunTransaction);

	class Private;
	friend class Private;
	Private *d;
};

// keep track of many open transactions
class StunTransactionPool : public QObject
{
	Q_OBJECT

public:
	StunTransactionPool(QObject *parent = 0);
	~StunTransactionPool();

	// you must start the transaction before inserting it
	void insert(StunTransaction *trans);

	void remove(StunTransaction *trans);

	// note: not DOR-DS safe.  this will either emit signals and return
	//   true, or not emit signals and return false.
	bool writeIncomingMessage(const StunMessage &msg);

signals:
	void retransmit(XMPP::StunTransaction *trans);

	// when these are emitted, the transaction is removed from the pool
	void finished(XMPP::StunTransaction *trans, const XMPP::StunMessage &response);
	void error(XMPP::StunTransaction *trans, XMPP::StunTransaction::Error error);

private:
	Q_DISABLE_COPY(StunTransactionPool);

	class Private;
	friend class Private;
	Private *d;
};

}

#endif
