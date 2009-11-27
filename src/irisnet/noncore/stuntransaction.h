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
#include <QHostAddress>

namespace QCA {
	class SecureArray;
}

namespace XMPP {

class StunMessage;

class StunTransactionPrivate;
class StunTransactionPool;
class StunTransactionPoolPrivate;

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
		ErrorTimeout,
		ErrorAuth
	};

	StunTransaction(QObject *parent = 0);
	~StunTransaction();

	// toAddress/toPort are optional, to associate this request to a
	//   specific endpoint
	// note: not DOR-DS safe.  this function will cause the pool's
	//   outgoingMessage() signal to be emitted.
	void start(StunTransactionPool *pool, const QHostAddress &toAddress = QHostAddress(), int toPort = -1);

	// pass message with class unset.  use transaction id from the
	//   createMessage signal.
	void setMessage(const StunMessage &request);

	// transmission/timeout parameters, from RFC 5389.  by default,
	//   they are set to the recommended values from the RFC.
	void setRTO(int i);
	void setRc(int i);
	void setRm(int i);
	void setTi(int i);

	void setShortTermUsername(const QString &username);
	void setShortTermPassword(const QString &password);

signals:
	// you must use a direct connection with this signal and call
	//   setMessage() in the slot.  this signal may occur many times
	//   before the StunTransaction completes, and you must recreate the
	//   message every time using the new transactionId.
	void createMessage(const QByteArray &transactionId);

	void finished(const XMPP::StunMessage &response);
	void error(XMPP::StunTransaction::Error error);

private:
	Q_DISABLE_COPY(StunTransaction)

	friend class StunTransactionPool;
	friend class StunTransactionPoolPrivate;

	friend class StunTransactionPrivate;
	StunTransactionPrivate *d;
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

	// note: not DOR-DS safe.  this will either cause transactions to emit
	//   signals and return true, or not cause signals and return false.
	bool writeIncomingMessage(const StunMessage &msg, const QHostAddress &addr = QHostAddress(), int port = -1);
	bool writeIncomingMessage(const QByteArray &packet, const QHostAddress &addr = QHostAddress(), int port = -1);

	void setLongTermAuthEnabled(bool enabled);

	QString realm() const;
	void setUsername(const QString &username);
	void setPassword(const QCA::SecureArray &password);
	void setRealm(const QString &realm);
	void continueAfterParams();

	// for use with stun indications
	QByteArray generateId() const;

signals:
	// note: not DOR-SS safe.  writeIncomingMessage() must not be called
	//   during this signal.
	//
	// why do we need this restriction?  long explanation: since
	//   outgoingMessage() can be emitted as a result of calling a
	//   transaction's start(), and calling writeIncomingMessage() could
	//   result in a transaction completing, then calling
	//   writeIncomingMessage() during outgoingMessage() could cause
	//   a transaction's finished() or error() signals to emit during
	//   start(), which would violate DOR-DS.
	void outgoingMessage(const QByteArray &packet, const QHostAddress &addr, int port);

	void needAuthParams();

private:
	Q_DISABLE_COPY(StunTransactionPool)

	friend class StunTransaction;
	friend class StunTransactionPrivate;

	friend class StunTransactionPoolPrivate;
	StunTransactionPoolPrivate *d;
};

}

#endif
