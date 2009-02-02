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

#include "ice176.h"

#include <QTimer>
#include <QtCrypto>
#include "stuntransaction.h"
#include "stunbinding.h"
#include "stunallocate.h"
#include "icelocaltransport.h"

namespace XMPP {

static QChar randomPrintableChar()
{
	// 0-25 = a-z
	// 26-51 = A-Z
	// 52-61 = 0-9

	uchar c = QCA::Random::randomChar() % 62;
	if(c <= 25)
		return 'a' + c;
	else if(c <= 51)
		return 'A' + (c - 26);
	else
		return '0' + (c - 52);
}

static QString randomCredential(int len)
{
	QString out;
	for(int n = 0; n < len; ++n)
		out += randomPrintableChar();
	return out;
}

static int calc_priority(int typePref, int localPref, int componentId)
{
	Q_ASSERT(typePref >= 0 && typePref <= 126);
	Q_ASSERT(localPref >= 0 && localPref <= 65535);
	Q_ASSERT(componentId >= 1 && componentId <= 256);

	int priority = (1 << 24) * typePref;
	priority += (1 << 8) * localPref;
	priority += (256 - componentId);
	return priority;
}

class Ice176::Private : public QObject
{
	Q_OBJECT

public:
	enum CandidatePairState
	{
		PWaiting,
		PInProgress,
		PSucceeded,
		PFailed,
		PFrozen
	};

	enum CandidateType
	{
		HostType,
		PeerReflexiveType,
		ServerReflexiveType,
		RelayedType
	};

	enum CheckListState
	{
		LRunning,
		LCompleted,
		LFailed
	};

	class TransportAddress
	{
	public:
		QHostAddress addr;
		int port;

		TransportAddress() :
			port(-1)
		{
		}
	};

	class CandidateInfo
	{
	public:
		TransportAddress addr;
		CandidateType type;
		int priority;
		QString foundation;
		int componentId;
		TransportAddress base;
		TransportAddress related;
		int network;
	};

	class CandidatePair
	{
	public:
		CandidateInfo local, remote;
		bool isDefault;
		bool isValid;
		bool isNominated;
		CandidatePairState state;
	};

	class CheckList
	{
	public:
		QList<CandidatePair> pairs;
		CheckListState state;
	};

	class LocalTransport
	{
	public:
		IceLocalTransport *sock;
		QTimer *t; // for cutting stun request short
		int addrAt; // for calculating foundation, not great
		int network;
		bool isVpn;
		int componentId;
		bool started;
		bool use_stun;
		bool stun_finished;

		LocalTransport() :
			sock(0),
			t(0),
			addrAt(-1),
			network(-1),
			isVpn(false),
			componentId(-1),
			started(false),
			use_stun(false),
			stun_finished(false)
		{
		}
	};

	Ice176 *q;
	Ice176::Mode mode;
	int basePort;
	int componentCount;
	QList<Ice176::LocalAddress> localAddrs;
	QList<Ice176::ExternalAddress> extAddrs;
	Ice176::StunServiceType stunType;
	QHostAddress stunAddr;
	int stunPort;
	QString stunUser;
	QCA::SecureArray stunPass;
	QString localUser, localPass;
	QString peerUser, peerPass;
	//StunTransactionPool *pool;
	QList<LocalTransport*> localTransports;
	QList<CandidateInfo> localCandidates;

	Private(Ice176 *_q) :
		QObject(_q),
		q(_q),
		basePort(-1),
		componentCount(0)
	{
		//pool = new StunTransactionPool(StunTransaction::Udp, this);
		//connect(pool, SIGNAL(retransmit(XMPP::StunTransaction *)), SLOT(pool_retransmit(XMPP::StunTransaction *)));
	}

	~Private()
	{
		for(int n = 0; n < localTransports.count(); ++n)
		{
			delete localTransports[n]->sock;

			QTimer *t = localTransports[n]->t;
			t->disconnect(this);
			t->setParent(0);
			t->deleteLater();
		}

		qDeleteAll(localTransports);
	}

	// localPref is the priority of the network interface being used for
	//   this candidate.  the value must be between 0-65535 and different
	//   interfaces must have different values.  if there is only one
	//   interface, the value should be 65535.
	static int choose_default_priority(CandidateType type, int localPref, bool isVpn, int componentId)
	{
		int typePref;
		if(type == HostType)
		{
			if(isVpn)
				typePref = 0;
			else
				typePref = 126;
		}
		else if(type == PeerReflexiveType)
			typePref = 110;
		else if(type == ServerReflexiveType)
			typePref = 100;
		else // RelayedType
			typePref = 0;

		return calc_priority(typePref, localPref, componentId);
	}

	static QString candidateType_to_string(CandidateType type)
	{
		QString out;
		switch(type)
		{
			case HostType: out = "host"; break;
			case PeerReflexiveType: out = "prflx"; break;
			case ServerReflexiveType: out = "srflx"; break;
			case RelayedType: out = "relay"; break;
			default: Q_ASSERT(0);
		}
		return out;
	}

	void start()
	{
		localUser = randomCredential(4);
		localPass = randomCredential(22);

		for(int n = 0; n < componentCount; ++n)
		{
			for(int i = 0; i < localAddrs.count(); ++i)
			{
				LocalTransport *lt = new LocalTransport;
				lt->sock = new IceLocalTransport(this);
				connect(lt->sock, SIGNAL(started()), SLOT(lt_started()));
				connect(lt->sock, SIGNAL(stopped()), SLOT(lt_stopped()));
				connect(lt->sock, SIGNAL(stunFinished()), SLOT(lt_stunFinished()));
				connect(lt->sock, SIGNAL(error(XMPP::IceLocalTransport::Error)), SLOT(lt_error(XMPP::IceLocalTransport::Error)));
				connect(lt->sock, SIGNAL(readyRead(XMPP::IceLocalTransport::TransmitPath)), SLOT(lt_readyRead(XMPP::IceLocalTransport::TransmitPath)));
				connect(lt->sock, SIGNAL(datagramsWritten(XMPP::IceLocalTransport::TransmitPath, int)), SLOT(lt_datagramsWritten(XMPP::IceLocalTransport::TransmitPath, int)));
				lt->addrAt = i;
				lt->network = localAddrs[i].network;
				lt->isVpn = localAddrs[i].isVpn;
				lt->componentId = n + 1;
				localTransports += lt;
				int port = (basePort != -1) ? basePort + n : -1;
				lt->sock->start(localAddrs[i].addr, port);
			}
		}
	}

	void tryFinishGather()
	{
		bool allReady = true;
		foreach(const LocalTransport *lt, localTransports)
		{
			if(!lt->started || (lt->use_stun && !lt->stun_finished))
			{
				allReady = false;
				break;
			}
		}

		if(allReady)
		{
			emit q->started();

			// FIXME: DOR-SS
			QList<Ice176::Candidate> list;
			foreach(const CandidateInfo &ci, localCandidates)
			{
				Ice176::Candidate c;
				c.component = ci.componentId;
				c.foundation = ci.foundation;
				c.generation = 0;
				c.id = QString(); // FIXME
				c.ip = ci.addr.addr;
				c.network = ci.network;
				c.port = ci.addr.port;
				c.priority = ci.priority;
				c.protocol = "udp";
				if(ci.type != HostType)
				{
					c.rel_addr = ci.base.addr;
					c.rel_port = ci.base.port;
				}
				else
				{
					c.rel_addr = QHostAddress();
					c.rel_port = -1;
				}
				c.rem_addr = QHostAddress();
				c.rem_port = -1;
				c.type = candidateType_to_string(ci.type);
				list += c;
			}
			if(!list.isEmpty())
				emit q->localCandidatesReady(list);
		}
	}

public slots:
	void lt_started()
	{
		IceLocalTransport *sock = (IceLocalTransport *)sender();
		int at = -1;
		for(int n = 0; n < localTransports.count(); ++n)
		{
			if(localTransports[n]->sock == sock)
			{
				at = n;
				break;
			}
		}
		if(at == -1)
			return;

		LocalTransport *lt = localTransports[at];
		lt->started = true;

		CandidateInfo ci;
		ci.addr.addr = lt->sock->localAddress();
		ci.addr.port = lt->sock->localPort();
		ci.type = HostType;
		ci.componentId = lt->componentId;
		ci.priority = choose_default_priority(ci.type, 65535 - lt->addrAt, lt->isVpn, ci.componentId);
		ci.foundation = QString::number(lt->addrAt);
		ci.base = ci.addr;
		ci.network = lt->network;
		localCandidates += ci;

		if(!stunAddr.isNull())
		{
			lt->use_stun = true;
			if(stunType == Ice176::Relay)
				lt->sock->setStunService(IceLocalTransport::Relay, stunAddr, stunPort);
			else
				lt->sock->setStunService(IceLocalTransport::Basic, stunAddr, stunPort);

			// reduce gathering of STUN candidates to 4 seconds
			//   when trickle mode is disabled
			lt->t = new QTimer(this);
			connect(lt->t, SIGNAL(timeout()), SLOT(lt_timeout()));
			lt->t->setSingleShot(true);
			lt->t->start(4000);

			printf("starting stun\n");
			lt->sock->stunStart();
			return;
		}

		tryFinishGather();
	}

	void lt_stopped()
	{
		// TODO
		printf("lt_stopped\n");
	}

	void lt_stunFinished()
	{
		printf("lt_stunFinished\n");

		IceLocalTransport *sock = (IceLocalTransport *)sender();
		int at = -1;
		for(int n = 0; n < localTransports.count(); ++n)
		{
			if(localTransports[n]->sock == sock)
			{
				at = n;
				break;
			}
		}
		if(at == -1)
			return;

		LocalTransport *lt = localTransports[at];

		// already marked as finished?  this can happen if we timed
		//   out the operation from earlier.  in that case just
		//   ignore the event
		if(lt->stun_finished)
			return;

		lt->stun_finished = true;

		if(!lt->sock->serverReflexiveAddress().isNull())
		{
			CandidateInfo ci;
			ci.addr.addr = lt->sock->serverReflexiveAddress();
			ci.addr.port = lt->sock->serverReflexivePort();
			ci.type = ServerReflexiveType;
			ci.componentId = lt->componentId;
			ci.priority = choose_default_priority(ci.type, 65535 - lt->addrAt, lt->isVpn, ci.componentId);
			ci.foundation = QString::number(lt->addrAt) + 's';
			ci.base.addr = lt->sock->localAddress();
			ci.base.port = lt->sock->localPort();
			ci.network = lt->network;
			localCandidates += ci;
		}

		// TODO: relayed candidate

		tryFinishGather();
	}

	void lt_error(XMPP::IceLocalTransport::Error e)
	{
		// TODO
		Q_UNUSED(e);
		printf("lt_error\n");
	}

	void lt_readyRead(XMPP::IceLocalTransport::TransmitPath path)
	{
		// TODO
		Q_UNUSED(path);
	}

	void lt_datagramsWritten(XMPP::IceLocalTransport::TransmitPath path, int count)
	{
		// TODO
		Q_UNUSED(path);
		Q_UNUSED(count);
	}

	void lt_timeout()
	{
		QTimer *t = (QTimer *)sender();
		int at = -1;
		for(int n = 0; n < localTransports.count(); ++n)
		{
			if(localTransports[n]->t == t)
			{
				at = n;
				break;
			}
		}
		if(at == -1)
			return;

		LocalTransport *lt = localTransports[at];
		lt->stun_finished = true;

		// TODO: delete lt->t ?

		tryFinishGather();
	}

	void pool_retransmit(XMPP::StunTransaction *trans)
	{
		// TODO
		Q_UNUSED(trans);
	}
};

Ice176::Ice176(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

Ice176::~Ice176()
{
	delete d;
}

void Ice176::reset()
{
	// TODO
}

void Ice176::setBasePort(int port)
{
	d->basePort = port;
}

void Ice176::setLocalAddresses(const QList<LocalAddress> &addrs)
{
	// TODO: dedup
	d->localAddrs = addrs;
}

void Ice176::setExternalAddresses(const QList<ExternalAddress> &addrs)
{
	// TODO: dedup
	d->extAddrs.clear();
	foreach(const ExternalAddress &addr, addrs)
	{
		if(d->localAddrs.contains(addr.base))
			d->extAddrs += addrs;
	}
}

void Ice176::setStunService(StunServiceType type, const QHostAddress &addr, int port)
{
	d->stunType = type;
	d->stunAddr = addr;
	d->stunPort = port;
}

void Ice176::setComponentCount(int count)
{
	d->componentCount = count;
}

void Ice176::start(Mode mode)
{
	d->mode = mode;
	d->start();
}

QString Ice176::localUfrag() const
{
	return d->localUser;
}

QString Ice176::localPassword() const
{
	return d->localPass;
}

void Ice176::setPeerUfrag(const QString &ufrag)
{
	d->peerUser = ufrag;
}

void Ice176::setPeerPassword(const QString &pass)
{
	d->peerPass = pass;
}

void Ice176::setStunUsername(const QString &user)
{
	d->stunUser = user;
}

void Ice176::setStunPassword(const QCA::SecureArray &pass)
{
	d->stunPass = pass;
}

void Ice176::addRemoteCandidates(const QList<Candidate> &list)
{
	// TODO
	Q_UNUSED(list);
}

bool Ice176::hasPendingDatagrams(int componentIndex) const
{
	// TODO
	Q_UNUSED(componentIndex);
	return false;
}

QByteArray Ice176::readDatagram(int componentIndex)
{
	// TODO
	Q_UNUSED(componentIndex);
	return QByteArray();
}

void Ice176::writeDatagram(int componentIndex, const QByteArray &datagram)
{
	// TODO
	Q_UNUSED(componentIndex);
	Q_UNUSED(datagram);
}

}

#include "ice176.moc"
