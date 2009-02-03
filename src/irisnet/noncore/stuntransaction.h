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

namespace QCA {
	class SecureArray;
}

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
	// FIXME: stuser/stpass are a hack
	void start(Mode mode, const StunMessage &request, const QString &stuser = QString(), const QString &stpass = QString());

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
	Q_DISABLE_COPY(StunTransaction)

	class Private;
	friend class Private;
	Private *d;
};

// keep track of many open transactions.  note that retransmit() may be
//   emitted as a direct result of calling certain member functions of this
//   class as well as any other class that might use it (such as StunBinding).
//   so, be careful with what you do in your retransmit slot.
class StunTransactionPool : public QObject
{
	Q_OBJECT

public:
	StunTransactionPool(StunTransaction::Mode mode, QObject *parent = 0);
	~StunTransactionPool();

	StunTransaction::Mode mode() const;

	// generate a random id not used by any transaction in the pool
	QByteArray generateId() const;

	// you must start the transaction before inserting it.
	// note: not DOR-DS safe.  this function will cause retransmit() to be
	//   emitted.
	void insert(StunTransaction *trans);

	void remove(StunTransaction *trans);

	// note: not DOR-DS safe.  this will either cause transactions to emit
	//   signals and return true, or not cause signals and return false.
	bool writeIncomingMessage(const StunMessage &msg);

	QString realm() const;
	void setUsername(const QString &username);
	void setPassword(const QCA::SecureArray &password);
	void setRealm(const QString &realm);

	void setShortTermCredentialsEnabled(bool enabled);
	void continueAfterParams();

	QString username() const;
	QString password() const;

signals:
	// note: not DOR-SS safe.  writeIncomingMessage() must not be called
	//   during this signal.
	void retransmit(XMPP::StunTransaction *trans);

	void needAuthParams();

private:
	Q_DISABLE_COPY(StunTransactionPool)

	class Private;
	friend class Private;
	Private *d;
};

}

#endif
