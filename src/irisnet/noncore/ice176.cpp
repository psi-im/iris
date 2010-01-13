/*
 * Copyright (C) 2009,2010  Barracuda Networks, Inc.
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
#include <QUdpSocket>
#include <QtCrypto>
#include "stuntransaction.h"
#include "stunbinding.h"
#include "stunallocate.h"
#include "stunmessage.h"
#include "icelocaltransport.h"

namespace XMPP {

enum
{
	Direct,
	Relayed
};

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

static qint64 calc_pair_priority(int a, int b)
{
	qint64 priority = ((qint64)1 << 32) * qMin(a, b);
	priority += (qint64)2 * qMax(a, b);
	if(a > b)
		++priority;
	return priority;
}

class Ice176::Private : public QObject
{
	Q_OBJECT

public:
	enum CandidateType
	{
		HostType,
		PeerReflexiveType,
		ServerReflexiveType,
		RelayedType
	};

	enum CandidatePairState
	{
		PWaiting,
		PInProgress,
		PSucceeded,
		PFailed,
		PFrozen
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

		bool operator==(const TransportAddress &other) const
		{
			if(addr == other.addr && port == other.port)
				return true;
			else
				return false;
		}

		inline bool operator!=(const TransportAddress &other) const
		{
			return !operator==(other);
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
		QString id;
		int network;

		bool operator==(const CandidateInfo &other) const
		{
			if(addr == other.addr &&
				type == other.type &&
				priority == other.priority &&
				foundation == other.foundation &&
				componentId == other.componentId &&
				base == other.base &&
				related == other.related &&
				network == other.network)
			{
				return true;
			}
			else
				return false;
		}

		inline bool operator!=(const CandidateInfo &other) const
		{
			return !operator==(other);
		}
	};

	class CandidatePair
	{
	public:
		CandidateInfo local, remote;
		bool isDefault;
		bool isValid;
		bool isNominated;
		CandidatePairState state;

		qint64 priority;
		QString foundation;

		StunBinding *binding;

		// FIXME: this is wrong i think, it should be in LocalTransport
		//   or such, to multiplex ids
		StunTransactionPool *pool;

		CandidatePair() :
			binding(0),
			pool(0)
		{
		}
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
		QUdpSocket *qsock;
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
	QList<LocalTransport*> localTransports;
	QList<CandidateInfo> localCandidates;
	CheckList checkList;
	QList< QList<QByteArray> > in;

	Private(Ice176 *_q) :
		QObject(_q),
		q(_q),
		basePort(-1),
		componentCount(0)
	{
	}

	~Private()
	{
		for(int n = 0; n < localTransports.count(); ++n)
		{
			delete localTransports[n]->sock;
			localTransports[n]->qsock->deleteLater();

			QTimer *t = localTransports[n]->t;
			if(t)
			{
				t->disconnect(this);
				t->setParent(0);
				t->deleteLater();
			}
		}

		qDeleteAll(localTransports);

		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			StunBinding *binding = checkList.pairs[n].binding;
			StunTransactionPool *pool = checkList.pairs[n].pool;

			delete binding;

			if(pool)
			{
				pool->disconnect(this);
				pool->setParent(0);
				pool->deleteLater();
			}
		}
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

	static int string_to_candidateType(const QString &in)
	{
		if(in == "host")
			return HostType;
		else if(in == "prflx")
			return PeerReflexiveType;
		else if(in == "srflx")
			return ServerReflexiveType;
		else if(in == "relay")
			return RelayedType;
		else
			return -1;
	}

	void start()
	{
		localUser = randomCredential(4);
		localPass = randomCredential(22);

		bool atLeastOneTransport = false;
		for(int n = 0; n < componentCount; ++n)
		{
			in += QList<QByteArray>();

			for(int i = 0; i < localAddrs.count(); ++i)
			{
				if(localAddrs[i].addr.protocol() != QAbstractSocket::IPv4Protocol)
				{
					printf("warning: skipping non-ipv4 address: %s\n", qPrintable(localAddrs[i].addr.toString()));
					continue;
				}

				int port = (basePort != -1) ? basePort + n : 0;

				QUdpSocket *qsock = new QUdpSocket(this);
				if(!qsock->bind(localAddrs[i].addr, port))
				{
					delete qsock;
					printf("warning: unable to bind to port %d\n", port);
					continue;
				}

				LocalTransport *lt = new LocalTransport;
				lt->qsock = qsock;
				lt->sock = new IceLocalTransport(this);
				connect(lt->sock, SIGNAL(started()), SLOT(lt_started()));
				connect(lt->sock, SIGNAL(stopped()), SLOT(lt_stopped()));
				connect(lt->sock, SIGNAL(addressesChanged()), SLOT(lt_addressesChanged()));
				connect(lt->sock, SIGNAL(error(int)), SLOT(lt_error(int)));
				connect(lt->sock, SIGNAL(readyRead(int)), SLOT(lt_readyRead(int)));
				connect(lt->sock, SIGNAL(datagramsWritten(int, int, const QHostAddress &, int)), SLOT(lt_datagramsWritten(int, int, const QHostAddress &, int)));
				lt->addrAt = i;
				lt->network = localAddrs[i].network;
				lt->isVpn = localAddrs[i].isVpn;
				lt->componentId = n + 1;
				localTransports += lt;

				lt->sock->start(qsock);

				atLeastOneTransport = true;
				printf("starting transport %s:%d for component %d\n", qPrintable(localAddrs[i].addr.toString()), port, lt->componentId);
			}
		}

		if(!atLeastOneTransport)
			QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection);
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
				c.id = ci.id;
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

	void addRemoteCandidates(const QList<Candidate> &list)
	{
		QList<CandidateInfo> remoteCandidates;
		foreach(const Candidate &c, list)
		{
			CandidateInfo ci;
			ci.addr.addr = c.ip;
			ci.addr.port = c.port;
			ci.type = (CandidateType)string_to_candidateType(c.type); // TODO: handle error
			ci.componentId = c.component;
			ci.priority = c.priority;
			ci.foundation = c.foundation;
			if(!c.rel_addr.isNull())
			{
				ci.base.addr = c.rel_addr;
				ci.base.port = c.rel_port;
			}
			ci.network = c.network;
			ci.id = c.id;
			remoteCandidates += ci;
		}

		printf("adding %d remote candidates\n", remoteCandidates.count());

		QList<CandidatePair> pairs;
		foreach(const CandidateInfo &lc, localCandidates)
		{
			foreach(const CandidateInfo &rc, remoteCandidates)
			{
				if(lc.componentId != rc.componentId)
					continue;

				CandidatePair pair;
				pair.state = PFrozen; // FIXME: setting state here may be wrong
				pair.local = lc;
				pair.remote = rc;
				pair.isDefault = false;
				pair.isValid = false;
				pair.isNominated = false;
				if(mode == Ice176::Initiator)
					pair.priority = calc_pair_priority(lc.priority, rc.priority);
				else
					pair.priority = calc_pair_priority(rc.priority, lc.priority);
				pairs += pair;
			}
		}

		printf("%d pairs\n", pairs.count());

		// combine pairs with existing, and sort
		pairs = checkList.pairs + pairs;
		checkList.pairs.clear();
		while(!pairs.isEmpty())
		{
			int at = -1;
			qint64 highest_priority = -1;
			for(int n = 0; n < pairs.count(); ++n)
			{
				if(n == 0 || pairs[n].priority > highest_priority)
				{
					at = n;
					highest_priority = pairs[n].priority;
				}
			}

			CandidatePair pair = pairs[at];
			pairs.removeAt(at);
			checkList.pairs += pair;
		}

		// pruning

		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			CandidatePair &pair = checkList.pairs[n];
			if(pair.local.type == ServerReflexiveType)
				pair.local.addr = pair.local.base;
		}

		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			CandidatePair &pair = checkList.pairs[n];
			printf("%d, %s:%d -> %s:%d\n", pair.local.componentId, qPrintable(pair.local.addr.addr.toString()), pair.local.addr.port, qPrintable(pair.remote.addr.addr.toString()), pair.remote.addr.port);

			bool found = false;
			for(int i = n - 1; i >= 0; --i)
			{
				if(pair.local == checkList.pairs[i].local && pair.remote == checkList.pairs[i].remote)
				{
					found = true;
					break;
				}
			}

			if(found)
			{
				checkList.pairs.removeAt(n);
				--n; // adjust position
			}
		}

		printf("%d after pruning\n", checkList.pairs.count());

		// set state
		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			CandidatePair &pair = checkList.pairs[n];

			// only initialize the new pairs
			if(pair.state != PFrozen)
				continue;

			pair.foundation = pair.local.foundation + pair.remote.foundation;

			// FIXME: for now we just do checks to everything immediately
			pair.state = PInProgress;

			int at = -1;
			for(int i = 0; i < localTransports.count(); ++i)
			{
				if(localTransports[i]->sock->localAddress() == pair.local.addr.addr && localTransports[i]->sock->localPort() == pair.local.addr.port)
				{
					at = i;
					break;
				}
			}
			Q_ASSERT(at != -1);

			LocalTransport *lt = localTransports[at];

			pair.pool = new StunTransactionPool(StunTransaction::Udp, this);
			connect(pair.pool, SIGNAL(outgoingMessage(const QByteArray &, const QHostAddress &, int)), SLOT(pool_outgoingMessage(const QByteArray &, const QHostAddress &, int)));
			//pair.pool->setUsername(peerUser + ':' + localUser);
			//pair.pool->setPassword(peerPass.toUtf8());

			pair.binding = new StunBinding(pair.pool);
			connect(pair.binding, SIGNAL(success()), SLOT(binding_success()));

			int prflx_priority = choose_default_priority(PeerReflexiveType, 65535 - lt->addrAt, lt->isVpn, pair.local.componentId);
			pair.binding->setPriority(prflx_priority);

			if(mode == Ice176::Initiator)
			{
				pair.binding->setIceControlling(0);
				pair.binding->setUseCandidate(true);
			}
			else
				pair.binding->setIceControlled(0);

			pair.binding->setShortTermUsername(peerUser + ':' + localUser);
			pair.binding->setShortTermPassword(peerPass);

			pair.binding->start();
		}
	}

public slots:
	void lt_started()
	{
		printf("lt_started\n");

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
		ci.id = randomCredential(10); // FIXME: ensure unique
		localCandidates += ci;

		int extAt = -1;
		for(int n = 0; n < extAddrs.count(); ++n)
		{
			if(extAddrs[n].base.addr == lt->sock->localAddress() && (extAddrs[n].portBase == -1 || extAddrs[n].portBase == lt->sock->localPort()))
			{
				extAt = n;
				break;
			}
		}
		if(extAt != -1)
		{
			CandidateInfo ci;
			ci.addr.addr = extAddrs[extAt].addr;
			ci.addr.port = (extAddrs[extAt].portBase != -1) ? extAddrs[extAt].portBase : lt->sock->localPort();
			ci.type = ServerReflexiveType;
			ci.componentId = lt->componentId;
			ci.priority = choose_default_priority(ci.type, 65535 - lt->addrAt, lt->isVpn, ci.componentId);
			ci.foundation = QString::number(lt->addrAt) + 'e';
			ci.base.addr = lt->sock->localAddress();
			ci.base.port = lt->sock->localPort();
			ci.network = lt->network;
			ci.id = randomCredential(10); // FIXME: ensure unique
			localCandidates += ci;
		}

		if(!stunAddr.isNull())
		{
			lt->use_stun = true;
			if(stunType == Ice176::Basic)
				lt->sock->setStunService(stunAddr, stunPort, IceLocalTransport::Basic);
			else if(stunType == Ice176::Relay)
				lt->sock->setStunService(stunAddr, stunPort, IceLocalTransport::Relay);
			else // Auto
				lt->sock->setStunService(stunAddr, stunPort, IceLocalTransport::Auto);

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

	void lt_addressesChanged()
	{
		printf("lt_addressesChanged\n");

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
		{
			printf("ignoring\n");
			return;
		}

		lt->t->stop();
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
			ci.id = randomCredential(10); // FIXME: ensure unique
			localCandidates += ci;
		}

		// TODO: relayed candidate

		tryFinishGather();
	}

	void lt_error(int e)
	{
		// TODO
		Q_UNUSED(e);
		printf("lt_error\n");
		for(int n = 0; n < localTransports.count(); ++n)
			localTransports[n]->sock->disconnect(this);
		emit q->error();
	}

	void lt_readyRead(int path)
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

		if(path == Direct)
		{
			while(lt->sock->hasPendingDatagrams(path))
			{
				QHostAddress fromAddr;
				int fromPort;
				QByteArray buf = lt->sock->readDatagram(path, &fromAddr, &fromPort);

				//printf("port %d: received packet (%d bytes)\n", lt->sock->localPort(), buf.size());

				QString requser = localUser + ':' + peerUser;
				QByteArray reqkey = localPass.toUtf8();

				StunMessage::ConvertResult result;
				StunMessage msg = StunMessage::fromBinary(buf, &result, StunMessage::MessageIntegrity | StunMessage::Fingerprint, reqkey);
				if(!msg.isNull() && (msg.mclass() == StunMessage::Request || msg.mclass() == StunMessage::Indication))
				{
					printf("received validated request or indication\n");
					QString user = QString::fromUtf8(msg.attribute(0x0006)); // USERNAME
					if(requser != user)
					{
						printf("user [%s] is wrong.  it should be [%s].  skipping\n", qPrintable(user), qPrintable(requser));
						continue;
					}

					if(msg.method() != 0x001)
					{
						printf("not a binding request.  skipping\n");
						continue;
					}

					StunMessage response;
					response.setClass(StunMessage::SuccessResponse);
					response.setMethod(0x001);
					response.setId(msg.id());

					quint16 port16 = fromPort;
					quint32 addr4 = fromAddr.toIPv4Address();
					QByteArray val(8, 0);
					quint8 *p = (quint8 *)val.data();
					const quint8 *magic = response.magic();
					p[0] = 0;
					p[1] = 0x01;
					p[2] = (port16 >> 8) & 0xff;
					p[2] ^= magic[0];
					p[3] = port16 & 0xff;
					p[3] ^= magic[1];
					p[4] = (addr4 >> 24) & 0xff;
					p[4] ^= magic[0];
					p[5] = (addr4 >> 16) & 0xff;
					p[5] ^= magic[1];
					p[6] = (addr4 >> 8) & 0xff;
					p[6] ^= magic[2];
					p[7] = addr4 & 0xff;
					p[7] ^= magic[3];

					QList<StunMessage::Attribute> list;
					StunMessage::Attribute attr;
					attr.type = 0x0020;
					attr.value = val;
					list += attr;

					response.setAttributes(list);

					QByteArray packet = response.toBinary(StunMessage::MessageIntegrity | StunMessage::Fingerprint, reqkey);
					lt->sock->writeDatagram(path, packet, fromAddr, fromPort);
				}
				else
				{
					QByteArray reskey = peerPass.toUtf8();
					StunMessage msg = StunMessage::fromBinary(buf, &result, StunMessage::MessageIntegrity | StunMessage::Fingerprint, reskey);
					if(!msg.isNull() && (msg.mclass() == StunMessage::SuccessResponse || msg.mclass() == StunMessage::ErrorResponse))
					{
						printf("received validated response\n");

						// FIXME: this is so gross and completely defeats the point of having pools
						for(int n = 0; n < checkList.pairs.count(); ++n)
						{
							CandidatePair &pair = checkList.pairs[n];
							if(pair.local.addr.addr == lt->sock->localAddress() && pair.local.addr.port == lt->sock->localPort())
								pair.pool->writeIncomingMessage(msg);
						}
					}
					else
					{
						//printf("received some non-stun or invalid stun packet\n");

						// FIXME: i don't know if this is good enough
						if(StunMessage::isProbablyStun(buf))
						{
							printf("unexpected stun packet (loopback?), skipping.\n");
							continue;
						}

						int at = -1;
						for(int n = 0; n < checkList.pairs.count(); ++n)
						{
							CandidatePair &pair = checkList.pairs[n];
							if(pair.local.addr.addr == lt->sock->localAddress() && pair.local.addr.port == lt->sock->localPort())
							{
								at = n;
								break;
							}
						}
						if(at == -1)
						{
							printf("the local transport does not seem to be associated with a candidate?!\n");
							continue;
						}

						int componentIndex = checkList.pairs[at].local.componentId - 1;
						//printf("packet is considered to be application data for component index %d\n", componentIndex);

						in[componentIndex] += buf;
						emit q->readyRead(componentIndex);
					}
				}
			}
		}
		else // Relayed
		{
			// TODO
		}
	}

	void lt_datagramsWritten(int path, int count, const QHostAddress &addr, int port)
	{
		// TODO
		Q_UNUSED(path);
		Q_UNUSED(count);
		Q_UNUSED(addr);
		Q_UNUSED(port);
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

	void pool_outgoingMessage(const QByteArray &packet, const QHostAddress &addr, int port)
	{
		Q_UNUSED(addr);
		Q_UNUSED(port);

		StunTransactionPool *pool = (StunTransactionPool *)sender();
		int at = -1;
		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			if(checkList.pairs[n].pool == pool)
			{
				at = n;
				break;
			}
		}
		if(at == -1)
			return;

		CandidatePair &pair = checkList.pairs[at];

		at = -1;
		for(int n = 0; n < localTransports.count(); ++n)
		{
			if(pair.local.addr.addr == localTransports[n]->sock->localAddress() && pair.local.addr.port == localTransports[n]->sock->localPort())
			{
				at = n;
				break;
			}
		}
		if(at == -1)
			return;

		LocalTransport *lt = localTransports[at];

		printf("connectivity check from %s:%d to %s:%d\n", qPrintable(pair.local.addr.addr.toString()), pair.local.addr.port, qPrintable(pair.remote.addr.addr.toString()), pair.remote.addr.port);
		lt->sock->writeDatagram(Direct, packet, pair.remote.addr.addr, pair.remote.addr.port);
	}

	void binding_success()
	{
		StunBinding *binding = (StunBinding *)sender();
		int at = -1;
		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			if(checkList.pairs[n].binding == binding)
			{
				at = n;
				break;
			}
		}
		if(at == -1)
			return;

		printf("check success\n");

		CandidatePair &pair = checkList.pairs[at];

		// TODO: if we were cool, we'd do something with the peer
		//   reflexive address received

		// TODO: we're also supposed to do triggered checks.  except
		//   that currently we check everything anyway so this is not
		//   relevant

		// check if there's a candidate already valid
		at = -1;
		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			if(checkList.pairs[n].local.componentId == pair.local.componentId && checkList.pairs[n].isValid)
			{
				at = n;
				break;
			}
		}

		pair.isValid = true;

		if(at == -1)
		{
			emit q->componentReady(pair.local.componentId - 1);
		}
		else
		{
			printf("component %d already active, not signalling\n", pair.local.componentId);
		}
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
		bool found = false;
		foreach(const LocalAddress &la, d->localAddrs)
		{
			if(la.addr == addr.base.addr)
			{
				found = true;
				break;
			}
		}

		if(found)
			d->extAddrs += addrs;
	}
}

void Ice176::setStunService(const QHostAddress &addr, int port, StunServiceType type)
{
	d->stunAddr = addr;
	d->stunPort = port;
	d->stunType = type;
}

void Ice176::setStunUsername(const QString &user)
{
	d->stunUser = user;
}

void Ice176::setStunPassword(const QCA::SecureArray &pass)
{
	d->stunPass = pass;
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

void Ice176::addRemoteCandidates(const QList<Candidate> &list)
{
	d->addRemoteCandidates(list);
}

bool Ice176::hasPendingDatagrams(int componentIndex) const
{
	return !d->in[componentIndex].isEmpty();
}

QByteArray Ice176::readDatagram(int componentIndex)
{
	return d->in[componentIndex].takeFirst();
}

void Ice176::writeDatagram(int componentIndex, const QByteArray &datagram)
{
	int at = -1;
	for(int n = 0; n < d->checkList.pairs.count(); ++n)
	{
		if(d->checkList.pairs[n].local.componentId - 1 == componentIndex && d->checkList.pairs[n].isValid)
		{
			at = n;
			break;
		}
	}
	if(at == -1)
		return;

	Private::CandidatePair &pair = d->checkList.pairs[at];

	at = -1;
	for(int n = 0; n < d->localTransports.count(); ++n)
	{
		if(d->localTransports[n]->sock->localAddress() == pair.local.addr.addr && d->localTransports[n]->sock->localPort() == pair.local.addr.port)
		{
			at = n;
			break;
		}
	}
	if(at == -1)
		return;

	Private::LocalTransport *lt = d->localTransports[at];

	lt->sock->writeDatagram(Direct, datagram, pair.remote.addr.addr, pair.remote.addr.port);

	// DOR-SR?
	QMetaObject::invokeMethod(this, "datagramsWritten", Qt::QueuedConnection, Q_ARG(int, componentIndex), Q_ARG(int, 1));
}

void Ice176::flagComponentAsLowOverhead(int componentIndex)
{
	// TODO
	Q_UNUSED(componentIndex);
}

}

#include "ice176.moc"
