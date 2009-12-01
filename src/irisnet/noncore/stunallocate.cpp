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
#include <QTimer>
#include <QtCrypto>
#include "objectsession.h"
#include "stunmessage.h"
#include "stuntypes.h"
#include "stuntransaction.h"

// permissions last 5 minutes, update them every 4 minutes
#define PERM_INTERVAL  (4 * 60 * 1000)

// channels last 10 minutes, update them every 9 minutes
#define CHAN_INTERVAL  (9 * 60 * 1000)

Q_DECLARE_METATYPE(XMPP::StunAllocate::Error)

namespace XMPP {

void releaseAndDeleteLater(QObject *owner, QObject *obj)
{
	obj->disconnect(owner);
	obj->setParent(0);
	obj->deleteLater();
}

/*class StunAllocateRefreshable : public QObject
{
	Q_OBJECT

private:
	QTimer *timer;

public:
	StunAllocateRefreshable(int interval, QObject *parent = 0) :
		QObject(parent)
	{
		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
		timer->setSingleShot(true);
		timer->setInterval(interval);
	}

	~StunAllocateRefreshable()
	{
		releaseAndDeleteLater(this, timer);
	}

	void restartTimer()
	{
		timer->start();
	}

signals:
	void needRefresh();

private slots:
	void timer_timeout()
	{
		emit needRefresh();
	}
};*/

class StunAllocatePermission : public QObject
{
	Q_OBJECT

public:
	QTimer *timer;
	StunTransactionPool *pool;
	StunTransaction *trans;
	QHostAddress addr;
	bool active;
	int channelId;

	StunAllocatePermission(StunTransactionPool *_pool, const QHostAddress &_addr) :
		QObject(_pool),
		pool(_pool),
		trans(0),
		addr(_addr),
		active(false),
		channelId(-1)
	{
		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
		timer->setSingleShot(true);
		timer->setInterval(PERM_INTERVAL);
	}

	~StunAllocatePermission()
	{
		cleanup();

		releaseAndDeleteLater(this, timer);
	}

	void start()
	{
		Q_ASSERT(!active);
		doTransaction();
	}

	void makeChannel(int id)
	{
	}

signals:
	void ready();
	void error(int e, const QString &reason);

private:
	void cleanup()
	{
		delete trans;
		trans = 0;

		timer->stop();

		active = false;
	}

	void doTransaction()
	{
		Q_ASSERT(!trans);
		trans = new StunTransaction(this);
		connect(trans, SIGNAL(createMessage(const QByteArray &)), SLOT(trans_createMessage(const QByteArray &)));
		connect(trans, SIGNAL(finished(const XMPP::StunMessage &)), SLOT(trans_finished(const XMPP::StunMessage &)));
		connect(trans, SIGNAL(error(XMPP::StunTransaction::Error)), SLOT(trans_error(XMPP::StunTransaction::Error)));
		trans->start(pool);
	}

	void restartTimer()
	{
		timer->start();
	}

private slots:
	void trans_createMessage(const QByteArray &transactionId)
	{
		StunMessage message;
		message.setMethod(StunTypes::CreatePermission);
		message.setId((const quint8 *)transactionId.data());

		QList<StunMessage::Attribute> list;

		{
			StunMessage::Attribute a;
			a.type = StunTypes::XOR_PEER_ADDRESS;
			a.value = StunTypes::createXorPeerAddress(addr, 0, message.magic(), message.id());
			list += a;
		}

		message.setAttributes(list);

		trans->setMessage(message);
	}

	void trans_finished(const XMPP::StunMessage &response)
	{
		delete trans;
		trans = 0;

		bool err = false;
		int code;
		QString reason;
		if(response.mclass() == StunMessage::ErrorResponse)
		{
			if(!StunTypes::parseErrorCode(response.attribute(StunTypes::ERROR_CODE), &code, &reason))
			{
				cleanup();
				emit error(StunAllocate::ErrorProtocol, "Unable to parse ERROR-CODE in error response.");
				return;
			}

			err = true;
		}

		if(err)
		{
			cleanup();

			if(code == StunTypes::InsufficientCapacity)
				emit error(StunAllocate::ErrorCapacity, reason);
			else
				emit error(StunAllocate::ErrorRejected, reason);

			return;
		}

		restartTimer();

		if(!active)
		{
			active = true;
			emit ready();
		}
	}

	void trans_error(XMPP::StunTransaction::Error e)
	{
		cleanup();

		if(e == XMPP::StunTransaction::ErrorTimeout)
			emit error(StunAllocate::ErrorTimeout, "Request timed out.");
		else
			emit error(StunAllocate::ErrorGeneric, "Generic transaction error.");
	}

	void timer_timeout()
	{
		doTransaction();
	}
};

/*class StunAllocateChannel : public StunAllocateRefreshable
{
	Q_OBJECT

public:
	QHostAddress addr;
	int num;
	bool refreshing;

	StunAllocateChannel(QObject *parent = 0) :
		StunAllocateRefreshable(CHAN_INTERVAL, parent),
		num(-1),
		refreshing(false)
	{
	}
};*/

class StunAllocate::Private : public QObject
{
	Q_OBJECT

public:
	enum DontFragmentState
	{
		DF_Unknown,
		DF_Supported,
		DF_Unsupported
	};

	enum State
	{
		Stopped,
		Starting,
		Started,
		Refreshing,
		Stopping
	};

	StunAllocate *q;
	StunTransactionPool *pool;
	StunTransaction *trans;
	State state;
	QString errorString;
	DontFragmentState dfState;
	QString clientSoftware, serverSoftware;
	QHostAddress reflexiveAddress, relayedAddress;
	int reflexivePort, relayedPort;
	StunMessage msg;
	int allocateLifetime;
	QTimer *allocateRefreshTimer;
	QList<QHostAddress> pendingPerms;
	QList<StunAllocatePermission*> perms;
	QList<QHostAddress> permsAddrs;
	ObjectSession sess;

	Private(StunAllocate *_q) :
		QObject(_q),
		q(_q),
		pool(0),
		trans(0),
		state(Stopped),
		dfState(DF_Unknown),
		sess(this)
	{
		qRegisterMetaType<StunAllocate::Error>();

		allocateRefreshTimer = new QTimer(this);
		connect(allocateRefreshTimer, SIGNAL(timeout()), SLOT(refresh()));
		allocateRefreshTimer->setSingleShot(true);
	}

	~Private()
	{
		cleanup();

		releaseAndDeleteLater(this, allocateRefreshTimer);
	}

	void start()
	{
		Q_ASSERT(state == Stopped);

		state = Starting;
		doTransaction();
	}

	void stop()
	{
		Q_ASSERT(state == Started);

		state = Stopping;
		doTransaction();
	}

	void setPermissions(const QList<QHostAddress> &newPerms)
	{
		int freeCount = 0;

		// removed?
		for(int n = 0; n < perms.count(); ++n)
		{
			bool found = false;
			for(int k = 0; k < newPerms.count(); ++k)
			{
				if(newPerms[k] == perms[n]->addr)
				{
					found = true;
					break;
				}
			}

			if(!found)
			{
				++freeCount;

				delete perms[n];
				perms.removeAt(n);
				--n; // adjust position
			}
		}

		if(freeCount > 0)
		{
			// removals count as a change, so emit the signal
			sess.defer(q, "permissionsChanged");

			// wake up inactive perms now that we've freed space
			for(int n = 0; n < perms.count(); ++n)
				perms[n]->start();
		}

		// added?
		for(int n = 0; n < newPerms.count(); ++n)
		{
			bool found = false;
			for(int k = 0; k < perms.count(); ++k)
			{
				if(perms[k]->addr == newPerms[n])
				{
					found = true;
					break;
				}
			}

			if(!found)
			{
				StunAllocatePermission *perm = new StunAllocatePermission(pool, newPerms[n]);
				connect(perm, SIGNAL(ready()), SLOT(perm_ready()));
				connect(perm, SIGNAL(error(int, const QString &)), SLOT(perm_error(int, const QString &)));
				perms += perm;
				perm->start();
			}
		}
	}

private:
	void cleanup()
	{
		sess.reset();

		delete trans;
		trans = 0;

		allocateRefreshTimer->stop();

		pendingPerms.clear();
		qDeleteAll(perms);
		perms.clear();
		permsAddrs.clear();

		state = Stopped;
	}

	void doTransaction()
	{
		Q_ASSERT(!trans);
		trans = new StunTransaction(this);
		connect(trans, SIGNAL(createMessage(const QByteArray &)), SLOT(trans_createMessage(const QByteArray &)));
		connect(trans, SIGNAL(finished(const XMPP::StunMessage &)), SLOT(trans_finished(const XMPP::StunMessage &)));
		connect(trans, SIGNAL(error(XMPP::StunTransaction::Error)), SLOT(trans_error(XMPP::StunTransaction::Error)));
		trans->start(pool);
	}

	void restartRefreshTimer()
	{
		// refresh 1 minute shy of the lifetime
		allocateRefreshTimer->start((allocateLifetime - 60) * 1000);
	}

	bool updatePermsAddrs()
	{
		QList<QHostAddress> newList;

		for(int n = 0; n < perms.count(); ++n)
		{
			if(perms[n]->active)
				newList += perms[n]->addr;
		}

		if(newList == permsAddrs)
			return false;

		permsAddrs = newList;
		return true;
	}

private slots:
	void refresh()
	{
		Q_ASSERT(state == Started);

		state = Refreshing;
		doTransaction();
	}

	void trans_createMessage(const QByteArray &transactionId)
	{
		if(state == Starting)
		{
			// send Allocate request
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

			if(dfState == DF_Unknown)
			{
				StunMessage::Attribute a;
				a.type = StunTypes::DONT_FRAGMENT;
				list += a;
			}

			message.setAttributes(list);

			trans->setMessage(message);
		}
		else if(state == Stopping)
		{
			StunMessage message;
			message.setMethod(StunTypes::Refresh);
			message.setId((const quint8 *)transactionId.data());

			QList<StunMessage::Attribute> list;

			{
				StunMessage::Attribute a;
				a.type = StunTypes::LIFETIME;
				a.value = StunTypes::createLifetime(0);
				list += a;
			}

			message.setAttributes(list);

			trans->setMessage(message);
		}
		else if(state == Refreshing)
		{
			StunMessage message;
			message.setMethod(StunTypes::Refresh);
			message.setId((const quint8 *)transactionId.data());

			QList<StunMessage::Attribute> list;

			{
				StunMessage::Attribute a;
				a.type = StunTypes::LIFETIME;
				a.value = StunTypes::createLifetime(3600);
				list += a;
			}

			message.setAttributes(list);

			trans->setMessage(message);
		}
	}

	void trans_finished(const XMPP::StunMessage &response)
	{
		delete trans;
		trans = 0;

		bool error = false;
		int code;
		QString reason;
		if(response.mclass() == StunMessage::ErrorResponse)
		{
			if(!StunTypes::parseErrorCode(response.attribute(StunTypes::ERROR_CODE), &code, &reason))
			{
				cleanup();
				errorString = "Unable to parse ERROR-CODE in error response.";
				emit q->error(StunAllocate::ErrorProtocol);
				return;
			}

			error = true;
		}

		if(state == Starting)
		{
			if(error)
			{
				if(code == StunTypes::UnknownAttribute)
				{
					QList<quint16> typeList;
					if(!StunTypes::parseUnknownAttributes(response.attribute(StunTypes::UNKNOWN_ATTRIBUTES), &typeList))
					{
						cleanup();
						errorString = "Unable to parse UNKNOWN-ATTRIBUTES in 420 (Unknown Attribute) error response.";
						emit q->error(StunAllocate::ErrorProtocol);
						return;
					}

					if(typeList.contains(StunTypes::DONT_FRAGMENT))
					{
						dfState = DF_Unsupported;

						// stay in same state, try again
						doTransaction();
					}
					else
					{
						cleanup();
						errorString = reason;
						emit q->error(StunAllocate::ErrorGeneric);
					}

					return;
				}
				else if(code == StunTypes::AllocationMismatch)
				{
					cleanup();
					errorString = "437 (Allocation Mismatch).";
					emit q->error(StunAllocate::ErrorMismatch);
					return;
				}
				else if(code == StunTypes::InsufficientCapacity)
				{
					cleanup();
					errorString = reason;
					emit q->error(StunAllocate::ErrorCapacity);
					return;
				}
			}

			quint32 lifetime;
			if(!StunTypes::parseLifetime(response.attribute(StunTypes::LIFETIME), &lifetime))
			{
				cleanup();
				errorString = "Unable to parse LIFETIME.";
				emit q->error(StunAllocate::ErrorProtocol);
				return;
			}

			if(lifetime < 120)
			{
				cleanup();
				errorString = "LIFETIME is less than two minutes.  That is ridiculous.";
				emit q->error(StunAllocate::ErrorProtocol);
				return;
			}

			QHostAddress raddr;
			quint16 rport;
			if(!StunTypes::parseXorRelayedAddress(response.attribute(StunTypes::XOR_RELAYED_ADDRESS), response.magic(), response.id(), &raddr, &rport))
			{
				cleanup();
				errorString = "Unable to parse XOR-RELAYED-ADDRESS.";
				emit q->error(StunAllocate::ErrorProtocol);
				return;
			}

			QHostAddress saddr;
			quint16 sport;
			if(!StunTypes::parseXorMappedAddress(response.attribute(StunTypes::XOR_MAPPED_ADDRESS), response.magic(), response.id(), &saddr, &sport))
			{
				cleanup();
				errorString = "Unable to parse XOR-MAPPED-ADDRESS.";
				emit q->error(StunAllocate::ErrorProtocol);
				return;
			}

			allocateLifetime = lifetime;
			relayedAddress = raddr;
			relayedPort = rport;
			reflexiveAddress = saddr;
			reflexivePort = sport;

			if(dfState == DF_Unknown)
				dfState = DF_Supported;

			state = Started;
			restartRefreshTimer();

			emit q->started();
		}
		else if(state == Stopping)
		{
			if(error)
			{
				// AllocationMismatch on session cancel doesn't count as an error
				if(code != StunTypes::AllocationMismatch)
				{
					cleanup();
					errorString = reason;
					emit q->error(StunAllocate::ErrorGeneric);
					return;
				}
			}

			// cleanup will set the state to Stopped
			cleanup();
			emit q->stopped();
		}
		else if(state == Refreshing)
		{
			if(error)
			{
				cleanup();
				errorString = reason;
				emit q->error(StunAllocate::ErrorRejected);
				return;
			}

			quint32 lifetime;
			if(!StunTypes::parseLifetime(response.attribute(StunTypes::LIFETIME), &lifetime))
			{
				cleanup();
				errorString = "Unable to parse LIFETIME.";
				emit q->error(StunAllocate::ErrorProtocol);
				return;
			}

			allocateLifetime = lifetime;

			state = Started;
			restartRefreshTimer();
		}
	}

	void perm_ready()
	{
		if(updatePermsAddrs())
			emit q->permissionsChanged();
	}

	void perm_error(int e, const QString &reason)
	{
		if(e == StunAllocate::ErrorCapacity)
		{
			// if we aren't allowed to make anymore permissions,
			//   don't consider this an error.  the perm stays
			//   in the list inactive.  we'll try it again if
			//   any perms get removed.
			return;
		}

		cleanup();
		errorString = reason;
		emit q->error((StunAllocate::Error)e);
	}

	void trans_error(XMPP::StunTransaction::Error e)
	{
		delete trans;
		trans = 0;

		cleanup();

		if(e == StunTransaction::ErrorTimeout)
		{
			errorString = "Request timed out.";
			emit q->error(StunAllocate::ErrorTimeout);
		}
		else
		{
			errorString = "Generic transaction error.";
			emit q->error(StunAllocate::ErrorGeneric);
		}
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
	return d->permsAddrs;
}

void StunAllocate::setPermissions(const QList<QHostAddress> &perms)
{
	d->setPermissions(perms);
}

int StunAllocate::packetHeaderOverhead(const QHostAddress &addr) const
{
	// TODO: support ChannelBind
	Q_UNUSED(addr);
	return 36; // overhead of STUN-based data packets
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

	return message.toBinary();
}

QByteArray StunAllocate::decode(const QByteArray &encoded, QHostAddress *addr, int *port)
{
	// TODO: support ChannelBind
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
	return d->errorString;
}

}

#include "stunallocate.moc"
