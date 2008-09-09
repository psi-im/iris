/*
 * Copyright (C) 2005-2008  Justin Karneges
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "irisnetplugin.h"

#include "objectsession.h"
#include "jdnsshared.h"
#include "netinterface.h"

//#define JDNS_DEBUG

Q_DECLARE_METATYPE(XMPP::NameRecord)
Q_DECLARE_METATYPE(XMPP::NameResolver::Error)
Q_DECLARE_METATYPE(XMPP::ServiceBrowser::Error)
Q_DECLARE_METATYPE(XMPP::ServiceResolver::Error)
Q_DECLARE_METATYPE(XMPP::ServiceLocalPublisher::Error)

namespace XMPP {

static NameRecord importJDNSRecord(const QJDns::Record &in)
{
	NameRecord out;
	switch(in.type)
	{
		case QJDns::A:     out.setAddress(in.address); break;
		case QJDns::Aaaa:  out.setAddress(in.address); break;
		case QJDns::Mx:    out.setMx(in.name, in.priority); break;
		case QJDns::Srv:   out.setSrv(in.name, in.port, in.priority, in.weight); break;
		case QJDns::Cname: out.setCname(in.name); break;
		case QJDns::Ptr:   out.setPtr(in.name); break;
		case QJDns::Txt:   out.setTxt(in.texts); break;
		case QJDns::Hinfo: out.setHinfo(in.cpu, in.os); break;
		case QJDns::Ns:    out.setNs(in.name); break;
		case 10:           out.setNull(in.rdata); break;
		default:
			return out;
	}
	out.setOwner(in.owner);
	out.setTtl(in.ttl);
	return out;
}

static QJDns::Record exportJDNSRecord(const NameRecord &in)
{
	QJDns::Record out;
	switch(in.type())
	{
		case NameRecord::A:
			out.type = QJDns::A;
			out.haveKnown = true;
			out.address = in.address();
			break;
		case NameRecord::Aaaa:
			out.type = QJDns::Aaaa;
			out.haveKnown = true;
			out.address = in.address();
			break;
		case NameRecord::Mx:
			out.type = QJDns::Mx;
			out.haveKnown = true;
			out.name = in.name();
			out.priority = in.priority();
			break;
		case NameRecord::Srv:
			out.type = QJDns::Srv;
			out.haveKnown = true;
			out.name = in.name();
			out.port = in.port();
			out.priority = in.priority();
			out.weight = in.weight();
			break;
		case NameRecord::Cname:
			out.type = QJDns::Cname;
			out.haveKnown = true;
			out.name = in.name();
			break;
		case NameRecord::Ptr:
			out.type = QJDns::Ptr;
			out.haveKnown = true;
			out.name = in.name();
			break;
		case NameRecord::Txt:
			out.type = QJDns::Txt;
			out.haveKnown = true;
			out.texts = in.texts();
			break;
		case NameRecord::Hinfo:
			out.type = QJDns::Hinfo;
			out.haveKnown = true;
			out.cpu = in.cpu();
			out.os = in.os();
			break;
		case NameRecord::Ns:
			out.type = QJDns::Ns;
			out.haveKnown = true;
			out.name = in.name();
			break;
		case NameRecord::Null:
			out.type = 10;
			out.rdata = in.rawData();
			break;
		default:
			return out;
	}
	out.owner = in.owner();
	out.ttl = in.ttl();
	return out;
}

static bool validServiceType(const QByteArray &in)
{
	// can't be empty, or start/end with a dot
	if(in.isEmpty() || in[0] == '.' || in[in.length() - 1] == '.')
		return false;

	// must contain exactly one dot
	int dotcount = 0;
	for(int n = 0; n < in.length(); ++n)
	{
		if(in[n] == '.')
		{
			++dotcount;

			// no need to count more than 2
			if(dotcount >= 2)
				break;
		}
	}
	if(dotcount != 1)
		return false;

	return true;
}

static QByteArray escapeDomainPart(const QByteArray &in)
{
	QByteArray out;
	for(int n = 0; n < in.length(); ++n)
	{
		if(in[n] == '\\')
			out += "\\\\";
		else if(in[n] == '.')
			out += "\\.";
		else
			out += in[n];
	}
	return out;
}

static QByteArray unescapeDomainPart(const QByteArray &in)
{
	QByteArray out;
	for(int n = 0; n < in.length(); ++n)
	{
		if(in[n] == '\\')
		{
			if(n + 1 >= in.length())
				return QByteArray();

			out += in[n + 1];
		}
		else
			out += in[n];
	}
	return out;
}

class IdManager
{
private:
	QSet<int> set;
	int at;

	inline void bump_at()
	{
		if(at == 0x7fffffff)
			at = 0;
		else
			++at;
	}

public:
	IdManager() :
		at(0)
	{
	}

	int reserveId()
	{
		while(1)
		{
			if(!set.contains(at))
			{
				int id = at;
				set.insert(id);
				bump_at();
				return id;
			}

			bump_at();
		}
	}

	void releaseId(int id)
	{
		set.remove(id);
	}
};

//----------------------------------------------------------------------------
// JDnsGlobal
//----------------------------------------------------------------------------
class JDnsGlobal : public QObject
{
	Q_OBJECT

public:
	JDnsSharedDebug db;
	JDnsShared *uni_net, *uni_local, *mul;
	QHostAddress mul_addr4, mul_addr6;
	NetInterfaceManager netman;
	QList<NetInterface*> ifaces;
	QTimer *updateTimer;

	JDnsGlobal() :
		netman(this)
	{
		uni_net = 0;
		uni_local = 0;
		mul = 0;

		qRegisterMetaType<NameRecord>();
		qRegisterMetaType<NameResolver::Error>();
		qRegisterMetaType<ServiceBrowser::Error>();
		qRegisterMetaType<ServiceResolver::Error>();
		qRegisterMetaType<ServiceLocalPublisher::Error>();

		connect(&db, SIGNAL(readyRead()), SLOT(jdns_debugReady()));

		updateTimer = new QTimer(this);
		connect(updateTimer, SIGNAL(timeout()), SLOT(updateMulticastInterfaces()));
		updateTimer->setSingleShot(true);
	}

	~JDnsGlobal()
	{
		updateTimer->disconnect(this);
		updateTimer->setParent(0);
		updateTimer->deleteLater();

		qDeleteAll(ifaces);

		QList<JDnsShared*> list;
		if(uni_net)
			list += uni_net;
		if(uni_local)
			list += uni_local;
		if(mul)
			list += mul;

		// calls shutdown on the list, waits for shutdownFinished, deletes
		JDnsShared::waitForShutdown(list);

		// get final debug
		jdns_debugReady();
	}

	JDnsShared *ensure_uni_net()
	{
		if(!uni_net)
		{
			uni_net = new JDnsShared(JDnsShared::UnicastInternet, this);
			uni_net->setDebug(&db, "U");
			bool ok4 = uni_net->addInterface(QHostAddress::Any);
			bool ok6 = uni_net->addInterface(QHostAddress::AnyIPv6);
			if(!ok4 && !ok6)
			{
				delete uni_net;
				uni_net = 0;
			}
		}
		return uni_net;
	}

	JDnsShared *ensure_uni_local()
	{
		if(!uni_local)
		{
			uni_local = new JDnsShared(JDnsShared::UnicastLocal, this);
			uni_local->setDebug(&db, "L");
			bool ok4 = uni_local->addInterface(QHostAddress::Any);
			bool ok6 = uni_local->addInterface(QHostAddress::AnyIPv6);
			if(!ok4 && !ok6)
			{
				delete uni_local;
				uni_local = 0;
			}
		}
		return uni_local;
	}

	JDnsShared *ensure_mul()
	{
		if(!mul)
		{
			mul = new JDnsShared(JDnsShared::Multicast, this);
			mul->setDebug(&db, "M");

			connect(&netman, SIGNAL(interfaceAvailable(const QString &)), SLOT(iface_available(const QString &)));

			// get the current network interfaces.  this initial
			//   fetching should not trigger any calls to
			//   updateMulticastInterfaces().  only future
			//   activity should do that.
			foreach(const QString &id, netman.interfaces())
			{
				NetInterface *iface = new NetInterface(id, &netman);
				connect(iface, SIGNAL(unavailable()), SLOT(iface_unavailable()));
				ifaces += iface;
			}

			updateMulticastInterfaces();
		}
		return mul;
	}

private slots:
	void jdns_debugReady()
	{
		QStringList lines = db.readDebugLines();
#ifdef JDNS_DEBUG
		for(int n = 0; n < lines.count(); ++n)
			printf("jdns: %s\n", qPrintable(lines[n]));
#else
		Q_UNUSED(lines);
#endif
	}

	void iface_available(const QString &id)
	{
		NetInterface *iface = new NetInterface(id, &netman);
		connect(iface, SIGNAL(unavailable()), SLOT(iface_unavailable()));
		ifaces += iface;

		updateTimer->start(100);
	}

	void iface_unavailable()
	{
		NetInterface *iface = (NetInterface *)sender();
		ifaces.removeAll(iface);
		delete iface;

		updateTimer->start(100);
	}

	void updateMulticastInterfaces()
	{
		QHostAddress addr4 = QJDns::detectPrimaryMulticast(QHostAddress::Any);
		QHostAddress addr6 = QJDns::detectPrimaryMulticast(QHostAddress::AnyIPv6);

		updateMulticastInterface(&mul_addr4, addr4);
		updateMulticastInterface(&mul_addr6, addr6);
	}

private:
	void updateMulticastInterface(QHostAddress *curaddr, const QHostAddress &newaddr)
	{
		if(!(newaddr == *curaddr)) // QHostAddress doesn't have operator!=
		{
			if(!curaddr->isNull())
				mul->removeInterface(*curaddr);
			*curaddr = newaddr;
			if(!curaddr->isNull())
			{
				if(!mul->addInterface(*curaddr))
					*curaddr = QHostAddress();
			}
		}
	}
};

//----------------------------------------------------------------------------
// JDnsNameProvider
//----------------------------------------------------------------------------
class JDnsNameProvider : public NameProvider
{
	Q_OBJECT
	Q_INTERFACES(XMPP::NameProvider)

public:
	enum Mode { Internet, Local };

	JDnsGlobal *global;
	Mode mode;
	IdManager idman;
	ObjectSession sess;

	class Item
	{
	public:
		int id;
		JDnsSharedRequest *req;
		int type;
		bool longLived;
		ObjectSession sess;
		bool localResult;

		Item(QObject *parent = 0) :
			id(-1),
			req(0),
			sess(parent),
			localResult(false)
		{
		}

		~Item()
		{
			delete req;
		}
	};
	QList<Item*> items;

	static JDnsNameProvider *create(JDnsGlobal *global, Mode mode, QObject *parent = 0)
	{
		if(mode == Internet)
		{
			if(!global->ensure_uni_net())
				return 0;
		}
		else
		{
			if(!global->ensure_uni_local())
				return 0;
		}

		return new JDnsNameProvider(global, mode, parent);
	}

	JDnsNameProvider(JDnsGlobal *_global, Mode _mode, QObject *parent = 0) :
		NameProvider(parent)
	{
		global = _global;
		mode = _mode;
	}

	~JDnsNameProvider()
	{
		qDeleteAll(items);
	}

	Item *getItemById(int id)
	{
		for(int n = 0; n < items.count(); ++n)
		{
			if(items[n]->id == id)
				return items[n];
		}

		return 0;
	}

	Item *getItemByReq(JDnsSharedRequest *req)
	{
		for(int n = 0; n < items.count(); ++n)
		{
			if(items[n]->req == req)
				return items[n];
		}

		return 0;
	}

	void releaseItem(Item *i)
	{
		idman.releaseId(i->id);
		items.removeAll(i);
		delete i;
	}

	bool supportsSingle() const
	{
		return true;
	}

	bool supportsLongLived() const
	{
		if(mode == Local)
			return true;  // we support long-lived local queries
		else
			return false; // we do NOT support long-lived internet queries
	}

	virtual int resolve_start(const QByteArray &name, int qType, bool longLived)
	{
		if(mode == Internet)
		{
			// if query ends in .local, switch to local resolver
			if(name.right(6) == ".local" || name.right(7) == ".local.")
			{
				Item *i = new Item(this);
				i->id = idman.reserveId();
				i->longLived = longLived;
				items += i;
				i->sess.defer(this, "do_local", Q_ARG(int, i->id), Q_ARG(QByteArray, name));
				return i->id;
			}

			// we don't support long-lived internet queries
			if(longLived)
			{
				Item *i = new Item(this);
				i->id = idman.reserveId();
				items += i;
				i->sess.defer(this, "do_error", Q_ARG(int, i->id),
					Q_ARG(XMPP::NameResolver::Error, NameResolver::ErrorNoLongLived));
				return i->id;
			}

			// perform the query
			Item *i = new Item(this);
			i->id = idman.reserveId();
			i->req = new JDnsSharedRequest(global->uni_net);
			connect(i->req, SIGNAL(resultsReady()), SLOT(req_resultsReady()));
			i->type = qType;
			i->longLived = false;
			items += i;
			i->req->query(name, qType);
			return i->id;
		}
		else
		{
			Item *i = new Item(this);
			i->id = idman.reserveId();
			if(longLived)
			{
				if(!global->ensure_mul())
				{
					items += i;
					i->sess.defer(this, "do_error", Q_ARG(int, i->id),
						Q_ARG(XMPP::NameResolver::Error, NameResolver::ErrorNoLocal));
					return i->id;
				}

				i->req = new JDnsSharedRequest(global->mul);
				i->longLived = true;
			}
			else
			{
				i->req = new JDnsSharedRequest(global->uni_local);
				i->longLived = false;
			}
			connect(i->req, SIGNAL(resultsReady()), SLOT(req_resultsReady()));
			items += i;
			i->req->query(name, qType);
			return i->id;
		}
	}

	virtual void resolve_stop(int id)
	{
		Item *i = getItemById(id);
		Q_ASSERT(i);

		if(i->req)
			i->req->cancel();
		releaseItem(i);
	}

	virtual void resolve_localResultsReady(int id, const QList<XMPP::NameRecord> &results)
	{
		Item *i = getItemById(id);
		Q_ASSERT(i);
		Q_ASSERT(!i->localResult);

		i->localResult = true;
		i->sess.defer(this, "do_local_ready", Q_ARG(int, id),
			Q_ARG(QList<XMPP::NameRecord>, results));
	}

	virtual void resolve_localError(int id, XMPP::NameResolver::Error e)
	{
		Item *i = getItemById(id);
		Q_ASSERT(i);
		Q_ASSERT(!i->localResult);

		i->localResult = true;
		i->sess.defer(this, "do_local_error", Q_ARG(int, id),
			Q_ARG(XMPP::NameResolver::Error, e));
	}

private slots:
	void req_resultsReady()
	{
		JDnsSharedRequest *req = (JDnsSharedRequest *)sender();
		Item *i = getItemByReq(req);
		Q_ASSERT(i);

		int id = i->id;

		if(req->success())
		{
			QList<NameRecord> out;
			QList<QJDns::Record> results = req->results();
			for(int n = 0; n < results.count(); ++n)
			{
				// unless we are asking for all types, only
				//   accept the type we asked for
				if(i->type == QJDns::Any || results[n].type == i->type)
				{
					NameRecord rec = importJDNSRecord(results[n]);
					if(!rec.isNull())
						out += rec;
				}
			}
			if(!i->longLived)
				releaseItem(i);
			emit resolve_resultsReady(id, out);
		}
		else
		{
			JDnsSharedRequest::Error e = req->error();
			releaseItem(i);

			NameResolver::Error error = NameResolver::ErrorGeneric;
			if(e == JDnsSharedRequest::ErrorNXDomain)
				error = NameResolver::ErrorNoName;
			else if(e == JDnsSharedRequest::ErrorTimeout)
				error = NameResolver::ErrorTimeout;
			else // ErrorGeneric or ErrorNoNet
				error = NameResolver::ErrorGeneric;
			emit resolve_error(id, error);
		}
	}

	void do_error(int id, XMPP::NameResolver::Error e)
	{
		Item *i = getItemById(id);
		Q_ASSERT(i);

		releaseItem(i);
		emit resolve_error(id, e);
	}

	void do_local(int id, const QByteArray &name)
	{
		Item *i = getItemById(id);
		Q_ASSERT(i);

		// resolve_useLocal has two behaviors:
		// - if longlived, then it indicates a hand-off
		// - if non-longlived, then it indicates we want a subquery

		if(i->longLived)
			releaseItem(i);
		emit resolve_useLocal(id, name);
	}

	void do_local_ready(int id, const QList<XMPP::NameRecord> &results)
	{
		Item *i = getItemById(id);
		Q_ASSERT(i);

		// only non-longlived queries come through here, so we're done
		releaseItem(i);
		emit resolve_resultsReady(id, results);
	}

	void do_local_error(int id, XMPP::NameResolver::Error e)
	{
		Item *i = getItemById(id);
		Q_ASSERT(i);

		releaseItem(i);
		emit resolve_error(id, e);
	}
};

//----------------------------------------------------------------------------
// JDnsBrowse
//----------------------------------------------------------------------------
class JDnsBrowse : public QObject
{
	Q_OBJECT

public:
	QByteArray type, typeAndDomain;
	JDnsSharedRequest req;

	JDnsBrowse(JDnsShared *_jdns, QObject *parent = 0) :
		QObject(parent),
		req(_jdns, this)
	{
		connect(&req, SIGNAL(resultsReady()), SLOT(jdns_resultsReady()));
	}

	void start(const QByteArray &_type)
	{
		type = _type;
		Q_ASSERT(validServiceType(type));
		typeAndDomain = type + ".local.";
		req.query(typeAndDomain, QJDns::Ptr);
	}

signals:
	void available(const QByteArray &instance);
	void unavailable(const QByteArray &instance);

private:
	QByteArray parseInstanceName(const QByteArray &name)
	{
		// needs to be at least X + '.' + typeAndDomain
		if(name.length() < typeAndDomain.length() + 2)
			return QByteArray();

		// index of the '.' character
		int at = name.length() - typeAndDomain.length() - 1;

		if(name[at] != '.')
			return QByteArray();
		if(name.mid(at + 1) != typeAndDomain)
			return QByteArray();

		QByteArray friendlyName = unescapeDomainPart(name.mid(0, at));
		if(friendlyName.isEmpty())
			return QByteArray();

		return friendlyName;
	}

private slots:
	void jdns_resultsReady()
	{
		// ignore errors
		if(!req.success())
			return;

		QJDns::Record rec = req.results().first();

		Q_ASSERT(rec.type == QJDns::Ptr);

		QByteArray name = rec.name;
		QByteArray instance = parseInstanceName(name);
		if(instance.isEmpty())
			return;

		if(rec.ttl == 0)
		{
			emit unavailable(instance);
			return;
		}

		emit available(instance);
	}
};

//----------------------------------------------------------------------------
// JDnsServiceResolve
//----------------------------------------------------------------------------

// 5 second timeout waiting for both A and AAAA
// 8 second timeout waiting for at least one record
class JDnsServiceResolve : public QObject
{
	Q_OBJECT

public:
	enum SrvState
	{
		Srv               = 0,
		AddressWait       = 1,
		AddressFirstCome  = 2
	};

	JDnsSharedRequest reqtxt; // for TXT
	JDnsSharedRequest req;    // for SRV/A
	JDnsSharedRequest req6;   // for AAAA
	bool haveTxt;
	SrvState srvState;
	QTimer *opTimer;

	// out
	QList<QByteArray> attribs;
	QByteArray host;
	int port;
	bool have4, have6;
	QHostAddress addr4, addr6;

	JDnsServiceResolve(JDnsShared *_jdns, QObject *parent = 0) :
		QObject(parent),
		reqtxt(_jdns, this),
		req(_jdns, this),
		req6(_jdns, this)
	{
		connect(&reqtxt, SIGNAL(resultsReady()), SLOT(reqtxt_ready()));
		connect(&req, SIGNAL(resultsReady()), SLOT(req_ready()));
		connect(&req6, SIGNAL(resultsReady()), SLOT(req6_ready()));

		opTimer = new QTimer(this);
		connect(opTimer, SIGNAL(timeout()), SLOT(op_timeout()));
		opTimer->setSingleShot(true);
	}

	~JDnsServiceResolve()
	{
		opTimer->disconnect(this);
		opTimer->setParent(0);
		opTimer->deleteLater();
	}

	void start(const QByteArray name)
	{
		haveTxt = false;
		srvState = Srv;
		have4 = false;
		have6 = false;

		opTimer->start(8000);

		reqtxt.query(name, QJDns::Txt);
		req.query(name, QJDns::Srv);
	}

signals:
	void finished();
	void error();

private:
	void cleanup()
	{
		if(opTimer->isActive())
			opTimer->stop();
		if(!haveTxt)
			reqtxt.cancel();
		if(srvState == Srv || !have4)
			req.cancel();
		if(srvState >= AddressWait && !have6)
			req6.cancel();
	}

	bool tryDone()
	{
		// we're done when we have txt and addresses
		if(haveTxt && ( (have4 && have6) || (srvState == AddressFirstCome && (have4 || have6)) ))
		{
			cleanup();
			emit finished();
			return true;
		}

		return false;
	}

private slots:
	void reqtxt_ready()
	{
		if(!reqtxt.success())
		{
			cleanup();
			emit error();
			return;
		}

		QJDns::Record rec = reqtxt.results().first();
		reqtxt.cancel();

		if(rec.type != QJDns::Txt)
		{
			cleanup();
			emit error();
			return;
		}

		attribs.clear();
		if(!rec.texts.isEmpty())
		{
			// if there is only 1 text, it needs to be
			//   non-empty for us to care
			if(rec.texts.count() != 1 || !rec.texts[0].isEmpty())
				attribs = rec.texts;
		}

		haveTxt = true;

		tryDone();
	}

	void req_ready()
	{
		if(!req.success())
		{
			cleanup();
			emit error();
			return;
		}

		QJDns::Record rec = req.results().first();
		req.cancel();

		if(srvState == Srv)
		{
			// in Srv state, req is used for SRV records

			Q_ASSERT(rec.type == QJDns::Srv);

			host = rec.name;
			port = rec.port;

			srvState = AddressWait;
			opTimer->start(5000);

			req.query(host, QJDns::A);
			req6.query(host, QJDns::Aaaa);
		}
		else
		{
			// in the other states, req is used for A records

			Q_ASSERT(rec.type == QJDns::A);

			addr4 = rec.address;
			have4 = true;

			tryDone();
		}
	}

	void req6_ready()
	{
		if(!req6.success())
		{
			cleanup();
			emit error();
			return;
		}

		QJDns::Record rec = req6.results().first();
		req6.cancel();

		Q_ASSERT(rec.type == QJDns::Aaaa);

		addr6 = rec.address;
		have6 = true;

		tryDone();
	}

	void op_timeout()
	{
		if(srvState == Srv)
		{
			// timeout getting SRV.  it is possible that we could
			//   have obtained the TXT record, but if SRV times
			//   out then we consider the whole job to have
			//   failed.
			cleanup();
			emit error();
		}
		else if(srvState == AddressWait)
		{
			// timeout while waiting for both A and AAAA.  we now
			//   switch to the AddressFirstCome state, where an
			//   answer for either will do

			srvState = AddressFirstCome;

			// if we have at least one of these, we're done
			if(have4 || have6)
			{
				// well, almost.  we might still be waiting
				//   for the TXT record
				if(tryDone())
					return;
			}

			// if we are here, then it means we are missing TXT
			//   still, or we have neither A nor AAAA.

			// wait 3 more seconds
			opTimer->start(3000);
		}
		else // AddressFirstCome
		{
			// last chance!
			if(!tryDone())
			{
				cleanup();
				emit error();
			}
		}
	}
};

//----------------------------------------------------------------------------
// JDnsPublishAddresses
//----------------------------------------------------------------------------

// This class publishes A/AAAA records for the machine, using a derived
//   hostname (it will use QHostInfo::localHostName(), but append a unique
//   suffix if necessary).  If there is ever a record conflict, it will
//   republish under a unique name.
//
// The hostName() signal is emitted when a hostname is successfully
//   published as.  When there is a conflict, hostName() is emitted with
//   an empty value, and will again be emitted with a non-empty value
//   once the conflict is resolved.  A missing hostname is considered a
//   temporary problem, and so other publish operations that depend on a
//   hostname (SRV, etc) should block until a hostname is available.
class JDnsPublishAddresses : public QObject
{
	Q_OBJECT

public:
	bool started;
	JDnsSharedRequest pub_a;
	JDnsSharedRequest pub_aaaa;
	int counter;
	QByteArray host;
	bool success;
	bool haveA, haveAaaa;

	JDnsPublishAddresses(JDnsShared *_jdns, QObject *parent = 0) :
		QObject(parent),
		started(false),
		pub_a(_jdns, this),
		pub_aaaa(_jdns, this)
	{
		connect(&pub_a, SIGNAL(resultsReady()), SLOT(pub_a_ready()));
		connect(&pub_aaaa, SIGNAL(resultsReady()), SLOT(pub_aaaa_ready()));
	}

	void start()
	{
		counter = 1;
		success = false;
		haveA = false;
		haveAaaa = false;
		started = true;
		tryPublish();
	}

	bool isStarted() const
	{
		return started;
	}

signals:
	void hostName(const QByteArray &str);

private:
	void tryPublish()
	{
		QString me = QHostInfo::localHostName();
		if(counter > 1)
			me += QString(" (%1)").arg(counter);

		host = escapeDomainPart(me.toUtf8()) + ".local.";

		// A
		QJDns::Record rec;
		rec.type = QJDns::A;
		rec.owner = host;
		rec.ttl = 120;
		rec.haveKnown = true;
		rec.address = QHostAddress(); // null address, will be filled in
		pub_a.publish(QJDns::Unique, rec);

		// AAAA
		rec = QJDns::Record();
		rec.type = QJDns::Aaaa;
		rec.owner = host;
		rec.ttl = 120;
		rec.haveKnown = true;
		rec.address = QHostAddress(); // null address, will be filled in
		pub_aaaa.publish(QJDns::Unique, rec);
	}

	void tryDone()
	{
		if(haveA && haveAaaa)
		{
			success = true;
			emit hostName(host);
		}
	}

	void handleFail()
	{
		// we get here if we fail to publish at all, or if we
		//   successfully publish but then fail later on.  in the
		//   latter case it means we "lost" our host records.

		bool lostHost = success; // as in earlier publish success
		success = false;

		// if we lost a hostname with a suffix, or counter is
		//   at 99, then start counter over at 1 (no suffix).
		if((lostHost && counter > 1) || counter >= 99)
			counter = 1;
		else
			++counter;

		tryPublish();

		// only emit lost host signal once
		if(lostHost)
			emit hostName(QByteArray());
	}

private slots:
	void pub_a_ready()
	{
		if(pub_a.success())
		{
			haveA = true;
			tryDone();
		}
		else
		{
			haveA = false;
			haveAaaa = false;
			pub_aaaa.cancel();
			handleFail();
		}
	}

	void pub_aaaa_ready()
	{
		if(pub_aaaa.success())
		{
			haveAaaa = true;
			tryDone();
		}
		else
		{
			haveAaaa = false;
			haveA = false;
			pub_a.cancel();
			handleFail();
		}
	}
};

//----------------------------------------------------------------------------
// JDnsPublish
//----------------------------------------------------------------------------
class JDnsPublish;

class JDnsPublishExtra : public QObject
{
	Q_OBJECT

public:
	JDnsPublishExtra(JDnsPublish *_jdnsPub);
	~JDnsPublishExtra();

	void start(const QJDns::Record &_rec);
	void update(const QJDns::Record &_rec);

signals:
	void published();
	void error();

private:
	friend class JDnsPublish;

	JDnsPublish *jdnsPub;
	bool started;
	JDnsSharedRequest pub;
	QJDns::Record rec;
	bool have;
	bool need_update;
};

// This class publishes SRV/TXT/PTR for a service.  if a hostName is not
//   is not available (see JDnsPublishAddresses) then the publish action
//   will be deferred until one is available.  SRV and TXT are published
//   as unique records, and once they both succeed then the PTR record
//   is published.  once the PTR succeeds, then published() is emitted.
//   if a conflict occurs with any action, then the whole thing fails and
//   error() is emitted.  if, at any time, the hostName is lost, then
//   then the SRV operation is canceled, but no error is emitted.  when the
//   hostName is regained, then the SRV record is republished.
//
// It's important to note that published() is only emitted once ever, even
//   if a hostName change causes a republishing.  this way, hostName changes
//   are completely transparent.
class JDnsPublish : public QObject
{
	Q_OBJECT

public:
	JDnsShared *jdns;
	JDnsSharedRequest pub_srv;
	JDnsSharedRequest pub_txt;
	JDnsSharedRequest pub_ptr;

	bool have_srv, have_txt, have_ptr;
	bool need_update_txt;

	QByteArray fullname;
	QByteArray instance;
	QByteArray type;
	QByteArray host;
	int port;
	QList<QByteArray> attribs;

	QSet<JDnsPublishExtra*> extraList;

	JDnsPublish(JDnsShared *_jdns, QObject *parent = 0) :
		QObject(parent),
		jdns(_jdns),
		pub_srv(_jdns, this),
		pub_txt(_jdns, this),
		pub_ptr(_jdns, this)
	{
		connect(&pub_srv, SIGNAL(resultsReady()), SLOT(pub_srv_ready()));
		connect(&pub_txt, SIGNAL(resultsReady()), SLOT(pub_txt_ready()));
		connect(&pub_ptr, SIGNAL(resultsReady()), SLOT(pub_ptr_ready()));
	}

	~JDnsPublish()
	{
		qDeleteAll(extraList);
	}

	void start(const QString &_instance, const QByteArray &_type, const QByteArray &localHost, int _port, const QMap<QString,QByteArray> &attributes)
	{
		type = _type;
		Q_ASSERT(validServiceType(type));

		instance = escapeDomainPart(_instance.toUtf8());
		fullname = instance + '.' + type + ".local.";
		host = localHost;
		port = _port;
		attribs = makeTxtList(attributes);

		have_srv = false;
		have_txt = false;
		have_ptr = false;
		need_update_txt = false;

		// no host?  defer publishing till we have one
		if(host.isEmpty())
			return;

		doPublish();
	}

	void update(const QMap<QString,QByteArray> &attributes)
	{
		attribs = makeTxtList(attributes);

		// still publishing the initial txt?
		if(!have_txt)
		{
			// flag that we want to update once the publish
			//   succeeds.
			need_update_txt = true;
			return;
		}

		// no SRV, but have TXT?  this means we lost SRV due to
		//   a hostname change.
		if(!have_srv)
		{
			// in that case, revoke the TXT.  it'll get
			//   republished after SRV then.
			have_txt = false;
			pub_txt.cancel();
			return;
		}

		doPublishTxt();
	}

public slots:
	// pass empty host if host lost
	void hostChanged(const QByteArray &_host)
	{
		bool changed = (host != _host);

		if(changed)
		{
			host = _host;

			if(host.isEmpty())
			{
				// cancel srv record momentarily
				have_srv = false;
				pub_srv.cancel();
			}
			else
			{
				// we now have a host, publish
				doPublish();
			}
		}
	}

signals:
	void published();
	void error();

private:
	friend class JDnsPublishExtra;

	static QList<QByteArray> makeTxtList(const QMap<QString,QByteArray> &attributes)
	{
		QList<QByteArray> out;

		QMapIterator<QString,QByteArray> it(attributes);
		while(it.hasNext())
		{
			it.next();
			out += it.key().toLatin1() + '=' + it.value();
		}
		if(out.isEmpty())
			out += QByteArray();

		return out;
	}

	void doPublish()
	{
		// SRV
		QJDns::Record rec;
		rec.type = QJDns::Srv;
		rec.owner = fullname;
		rec.ttl = 120;
		rec.haveKnown = true;
		rec.name = host;
		rec.port = port;
		rec.priority = 0;
		rec.weight = 0;
		pub_srv.publish(QJDns::Unique, rec);

		// if we're just republishing SRV after losing/regaining
		//   our hostname, then TXT is already published
		if(!have_txt)
			doPublishTxt();

		// publish extra records as needed
		foreach(JDnsPublishExtra *extra, extraList)
		{
			if(!extra->have)
				doPublishExtra(extra);
		}
	}

	void doPublishTxt()
	{
		// TXT
		QJDns::Record rec;
		rec.type = QJDns::Txt;
		rec.owner = fullname;
		rec.ttl = 4500;
		rec.haveKnown = true;
		rec.texts = attribs;

		if(!have_txt)
			pub_txt.publish(QJDns::Unique, rec);
		else
			pub_txt.publishUpdate(rec);
	}

	void tryDone()
	{
		if(have_srv && have_txt)
		{
			// PTR
			QJDns::Record rec;
			rec.type = QJDns::Ptr;
			rec.owner = type + ".local.";
			rec.ttl = 4500;
			rec.haveKnown = true;
			rec.name = fullname;
			pub_ptr.publish(QJDns::Shared, rec);
		}
	}

	void cleanup()
	{
		foreach(JDnsPublishExtra *extra, extraList)
			cleanupExtra(extra);
		qDeleteAll(extraList);
		extraList.clear();

		have_srv = false;
		have_txt = false;
		have_ptr = false;
		pub_srv.cancel();
		pub_txt.cancel();
		pub_ptr.cancel();
	}

	void publishExtra(JDnsPublishExtra *extra)
	{
		Q_ASSERT(!extraList.contains(extra));

		connect(&extra->pub, SIGNAL(resultsReady()), SLOT(pub_extra_ready()));
		extraList += extra;

		// defer publishing until SRV is ready
		if(!have_srv)
			return;

		doPublishExtra(extra);
	}

	void publishExtraUpdate(JDnsPublishExtra *extra)
	{
		if(!extra->have)
		{
			extra->need_update = true;
			return;
		}

		if(!have_srv)
		{
			extra->have = false;
			extra->pub.cancel();
			return;
		}

		doPublishExtra(extra);
	}

	void unpublishExtra(JDnsPublishExtra *extra)
	{
		extraList.remove(extra);
	}

	void doPublishExtra(JDnsPublishExtra *extra)
	{
		if(!extra->have)
			extra->pub.publish(QJDns::Unique, extra->rec);
		else
			extra->pub.publishUpdate(extra->rec);
	}

	void cleanupExtra(JDnsPublishExtra *extra)
	{
		extra->pub.cancel();
		extra->disconnect(this);
		extra->started = false;
		extra->have = false;
	}

private slots:
	void pub_srv_ready()
	{
		if(pub_srv.success())
		{
			have_srv = true;
			tryDone();
		}
		else
		{
			cleanup();
			emit error();
		}
	}

	void pub_txt_ready()
	{
		if(pub_txt.success())
		{
			have_txt = true;

			if(need_update_txt)
			{
				need_update_txt = false;
				doPublishTxt();
			}

			tryDone();
		}
		else
		{
			cleanup();
			emit error();
		}
	}

	void pub_ptr_ready()
	{
		if(pub_ptr.success())
		{
			have_ptr = true;
			emit published();
		}
		else
		{
			cleanup();
			emit error();
		}
	}

	void pub_extra_ready()
	{
		JDnsSharedRequest *req = (JDnsSharedRequest *)sender();
		JDnsPublishExtra *extra = 0;
		foreach(JDnsPublishExtra *e, extraList)
		{
			if(&e->pub == req)
			{
				extra = e;
				break;
			}
		}
		Q_ASSERT(extra);

		if(extra->pub.success())
		{
			extra->have = true;

			if(extra->need_update)
			{
				extra->need_update = false;
				doPublishExtra(extra);
			}

			emit extra->published();
		}
		else
		{
			cleanupExtra(extra);
			emit extra->error();
		}
	}
};

JDnsPublishExtra::JDnsPublishExtra(JDnsPublish *_jdnsPub) :
	QObject(_jdnsPub),
	jdnsPub(_jdnsPub),
	started(false),
	pub(_jdnsPub->jdns, this)
{
}

JDnsPublishExtra::~JDnsPublishExtra()
{
	if(started)
		jdnsPub->unpublishExtra(this);
}

void JDnsPublishExtra::start(const QJDns::Record &_rec)
{
	rec = _rec;
	started = true;
	have = false;
	need_update = false;
	jdnsPub->publishExtra(this);
}

void JDnsPublishExtra::update(const QJDns::Record &_rec)
{
	rec = _rec;
	jdnsPub->publishExtraUpdate(this);
}

//----------------------------------------------------------------------------
// JDnsServiceProvider
//----------------------------------------------------------------------------
class BrowseItem
{
public:
	const int id;
	JDnsBrowse * const browse;
	ObjectSession *sess;

	BrowseItem(int _id, JDnsBrowse *_browse) :
		id(_id),
		browse(_browse),
		sess(0)
	{
	}

	~BrowseItem()
	{
		delete sess;
	}
};

class BrowseItemList
{
private:
	QSet<BrowseItem*> items;
	QHash<int,BrowseItem*> indexById;
	QHash<JDnsBrowse*,BrowseItem*> indexByBrowse;
	IdManager idman;

public:
	~BrowseItemList()
	{
		qDeleteAll(items);
	}

	int reserveId()
	{
		return idman.reserveId();
	}

	void insert(BrowseItem *item)
	{
		items.insert(item);
		indexById.insert(item->id, item);
		indexByBrowse.insert(item->browse, item);
	}

	void remove(BrowseItem *item)
	{
		indexById.remove(item->id);
		indexByBrowse.remove(item->browse);
		items.remove(item);
		if(item->id != -1)
			idman.releaseId(item->id);
		delete item;
	}

	BrowseItem *itemById(int id) const
	{
		return indexById.value(id);
	}

	BrowseItem *itemByBrowse(JDnsBrowse *browse) const
	{
		return indexByBrowse.value(browse);
	}
};

class ResolveItem
{
public:
	const int id;
	JDnsServiceResolve * const resolve;
	ObjectSession *sess;

	ResolveItem(int _id, JDnsServiceResolve *_resolve) :
		id(_id),
		resolve(_resolve),
		sess(0)
	{
	}

	~ResolveItem()
	{
		delete sess;
	}
};

class ResolveItemList
{
private:
	QSet<ResolveItem*> items;
	QHash<int,ResolveItem*> indexById;
	QHash<JDnsServiceResolve*,ResolveItem*> indexByResolve;
	IdManager idman;

public:
	~ResolveItemList()
	{
		qDeleteAll(items);
	}

	int reserveId()
	{
		return idman.reserveId();
	}

	void insert(ResolveItem *item)
	{
		items.insert(item);
		indexById.insert(item->id, item);
		indexByResolve.insert(item->resolve, item);
	}

	void remove(ResolveItem *item)
	{
		indexById.remove(item->id);
		indexByResolve.remove(item->resolve);
		items.remove(item);
		if(item->id != -1)
			idman.releaseId(item->id);
		delete item;
	}

	ResolveItem *itemById(int id) const
	{
		return indexById.value(id);
	}

	ResolveItem *itemByResolve(JDnsServiceResolve *resolve) const
	{
		return indexByResolve.value(resolve);
	}
};

class JDnsServiceProvider : public ServiceProvider
{
	Q_OBJECT

public:
	JDnsGlobal *global;

	// browse
	BrowseItemList browseItemList;
	QHash<QByteArray,ServiceInstance> items;

	// resolve
	ResolveItemList resolveItemList;

	// publish
	JDnsPublishAddresses *pub_addresses;

	QList<JDnsSharedRequest*> pubitems;
	QByteArray _servname;

	static JDnsServiceProvider *create(JDnsGlobal *global, QObject *parent = 0)
	{
		return new JDnsServiceProvider(global, parent);
	}

	JDnsServiceProvider(JDnsGlobal *_global, QObject *parent = 0) :
		ServiceProvider(parent),
		pub_addresses(0)
	{
		global = _global;

		pub_addresses = new JDnsPublishAddresses(global->ensure_mul(), this);
		connect(pub_addresses, SIGNAL(hostName(const QByteArray &)), SLOT(pub_addresses_hostName(const QByteArray &)));
		pub_addresses->start();
	}

	~JDnsServiceProvider()
	{
		qDeleteAll(pubitems);
	}

	virtual int browse_start(const QString &_type, const QString &_domain)
	{
		QString domain;
		if(_domain.isEmpty() || _domain == ".")
			domain = "local.";
		else
			domain = _domain;

		if(domain[domain.length() - 1] != '.')
			domain += '.';

		Q_ASSERT(domain.length() >= 2 && domain[domain.length() - 1] == '.');

		int id = browseItemList.reserveId();

		// no support for non-local domains
		if(domain != "local.")
		{
			BrowseItem *i = new BrowseItem(id, 0);
			i->sess = new ObjectSession(this);
			browseItemList.insert(i);
			i->sess->defer(this, "do_browse_error", Q_ARG(int, i->id),
				Q_ARG(XMPP::ServiceBrowser::Error, ServiceBrowser::ErrorNoWide));
			return i->id;
		}

		if(!global->ensure_mul())
		{
			BrowseItem *i = new BrowseItem(id, 0);
			i->sess = new ObjectSession(this);
			browseItemList.insert(i);
			i->sess->defer(this, "do_browse_error", Q_ARG(int, i->id),
				Q_ARG(XMPP::ServiceBrowser::Error, ServiceBrowser::ErrorNoLocal));
			return i->id;
		}

		QByteArray type = _type.toUtf8();
		if(!validServiceType(type))
		{
			BrowseItem *i = new BrowseItem(id, 0);
			i->sess = new ObjectSession(this);
			browseItemList.insert(i);
			i->sess->defer(this, "do_browse_error", Q_ARG(int, i->id),
				Q_ARG(XMPP::ServiceBrowser::Error, ServiceBrowser::ErrorGeneric));
			return i->id;
		}

		BrowseItem *i = new BrowseItem(id, new JDnsBrowse(global->mul, this));
		connect(i->browse, SIGNAL(available(const QByteArray &)), SLOT(jb_available(const QByteArray &)));
		connect(i->browse, SIGNAL(unavailable(const QByteArray &)), SLOT(jb_unavailable(const QByteArray &)));
		browseItemList.insert(i);
		i->browse->start(type);
		return i->id;
	}

	virtual void browse_stop(int id)
	{
		BrowseItem *i = browseItemList.itemById(id);
		Q_ASSERT(i);

		browseItemList.remove(i);
	}

	virtual int resolve_start(const QByteArray &name)
	{
		int id = resolveItemList.reserveId();

		if(!global->ensure_mul())
		{
			ResolveItem *i = new ResolveItem(id, 0);
			i->sess = new ObjectSession(this);
			resolveItemList.insert(i);
			i->sess->defer(this, "do_resolve_error", Q_ARG(int, i->id),
				Q_ARG(XMPP::ServiceResolver::Error, ServiceResolver::ErrorNoLocal));
			return i->id;
		}

		ResolveItem *i = new ResolveItem(id, new JDnsServiceResolve(global->mul, this));
		connect(i->resolve, SIGNAL(finished()), SLOT(jr_finished()));
		connect(i->resolve, SIGNAL(error()), SLOT(jr_error()));
		resolveItemList.insert(i);
		i->resolve->start(name);
		return i->id;
	}

	virtual void resolve_stop(int id)
	{
		ResolveItem *i = resolveItemList.itemById(id);
		Q_ASSERT(i);

		resolveItemList.remove(i);
	}

	virtual int publish_start(const QString &instance, const QString &type, int port, const QMap<QString,QByteArray> &attributes)
	{
		if(!global->ensure_mul())
		{
			// TODO
		}

		QString me = QHostInfo::localHostName();
		//QHostInfo hi = QHostInfo::fromName(me);
		QByteArray melocal = me.toLatin1() + ".local.";
		QByteArray servname = instance.toLatin1() + '.' + type.toLatin1() + ".local.";

		JDnsSharedRequest *req = new JDnsSharedRequest(global->mul);
		QJDns::Record rec;
		rec.type = QJDns::A;
		rec.owner = melocal;
		rec.ttl = 120;
		rec.haveKnown = true;
		rec.address = QHostAddress(); // null address, will be filled in
		req->publish(QJDns::Unique, rec);
		pubitems += req;

		/*JDnsSharedRequest *req = new JDnsSharedRequest(global->mul);
		QJDns::Record rec;
		rec.type = QJDns::Aaaa;
		rec.owner = melocal;
		rec.ttl = 120;
		rec.haveKnown = true;
		rec.address = QHostAddress(); // null address, will be filled in
		req->publish(QJDns::Unique, rec);
		pubitems += req;*/

		req = new JDnsSharedRequest(global->mul);
		rec = QJDns::Record();
		rec.type = QJDns::Srv;
		rec.owner = servname;
		rec.ttl = 120;
		rec.haveKnown = true;
		rec.name = melocal;
		rec.port = port;
		rec.priority = 0;
		rec.weight = 0;
		req->publish(QJDns::Unique, rec);
		pubitems += req;

		req = new JDnsSharedRequest(global->mul);
		rec = QJDns::Record();
		rec.type = QJDns::Txt;
		rec.owner = servname;
		rec.ttl = 4500;
		rec.haveKnown = true;
		QMapIterator<QString,QByteArray> it(attributes);
		while(it.hasNext())
		{
			it.next();
			rec.texts += it.key().toLatin1() + '=' + it.value();
		}
		if(rec.texts.isEmpty())
			rec.texts += QByteArray();
		req->publish(QJDns::Unique, rec);
		pubitems += req;

		req = new JDnsSharedRequest(global->mul);
		rec = QJDns::Record();
		rec.type = QJDns::Ptr;
		rec.owner = type.toLatin1() + ".local.";
		rec.ttl = 4500;
		rec.haveKnown = true;
		rec.name = servname;
		req->publish(QJDns::Shared, rec);
		pubitems += req;

		_servname = servname;

		QMetaObject::invokeMethod(this, "publish_published", Qt::QueuedConnection, Q_ARG(int, 1));

		return 1;
	}

	virtual void publish_update(int id, const QMap<QString,QByteArray> &attributes)
	{
		// TODO
		Q_UNUSED(id);
		Q_UNUSED(attributes);
	}

	virtual void publish_stop(int id)
	{
		// TODO
		Q_UNUSED(id);
	}

	virtual int publish_extra_start(int pub_id, const NameRecord &name)
	{
		// TODO
		Q_UNUSED(pub_id);

		JDnsSharedRequest *req = new JDnsSharedRequest(global->mul);
		QJDns::Record rec;
		rec.type = 10;
		rec.owner = _servname;
		rec.ttl = 4500;
		rec.rdata = name.rawData();
		req->publish(QJDns::Unique, rec);
		pubitems += req;

		QMetaObject::invokeMethod(this, "publish_extra_published", Qt::QueuedConnection, Q_ARG(int, 2));

		return 2;
	}

	virtual void publish_extra_update(int id, const NameRecord &name)
	{
		// TODO
		Q_UNUSED(id);
		Q_UNUSED(name);
	}

	virtual void publish_extra_stop(int id)
	{
		// TODO
		Q_UNUSED(id);
	}

private slots:
	void jb_available(const QByteArray &instance)
	{
		JDnsBrowse *jb = (JDnsBrowse *)sender();
		BrowseItem *i = browseItemList.itemByBrowse(jb);
		Q_ASSERT(i);

		QByteArray name = instance + '.' + jb->typeAndDomain;
		ServiceInstance si(QString::fromLatin1(instance), QString::fromLatin1(jb->type), "local.", QMap<QString,QByteArray>());
		items.insert(name, si);

		emit browse_instanceAvailable(i->id, si);
	}

	void jb_unavailable(const QByteArray &instance)
	{
		JDnsBrowse *jb = (JDnsBrowse *)sender();
		BrowseItem *i = browseItemList.itemByBrowse(jb);
		Q_ASSERT(i);

		QByteArray name = instance + '.' + jb->typeAndDomain;
		Q_ASSERT(items.contains(name));

		ServiceInstance si = items.value(name);
		items.remove(name);

		emit browse_instanceUnavailable(i->id, si);
	}

	void do_browse_error(int id, XMPP::ServiceBrowser::Error e)
	{
		BrowseItem *i = browseItemList.itemById(id);
		Q_ASSERT(i);

		browseItemList.remove(i);
		emit browse_error(id, e);
	}

	void jr_finished()
	{
		JDnsServiceResolve *jr = (JDnsServiceResolve *)sender();
		ResolveItem *i = resolveItemList.itemByResolve(jr);
		Q_ASSERT(i);

		// parse TXT list into attribute map
		QMap<QString,QByteArray> attribs;
		for(int n = 0; n < jr->attribs.count(); ++n)
		{
			const QByteArray &a = jr->attribs[n];
			QString key;
			QByteArray value;
			int x = a.indexOf('=');
			if(x != -1)
			{
				key = QString::fromLatin1(a.mid(0, x));
				value = a.mid(x + 1);
			}
			else
			{
				key = QString::fromLatin1(a);
			}

			attribs.insert(key, value);
		}

		// one of these must be true
		Q_ASSERT(jr->have4 || jr->have6);

		QList<ResolveResult> results;
		if(jr->have6)
		{
			ResolveResult r;
			r.attributes = attribs;
			r.address = jr->addr6;
			r.port = jr->port;
			r.hostName = jr->host;
			results += r;
		}
		if(jr->have4)
		{
			ResolveResult r;
			r.attributes = attribs;
			r.address = jr->addr4;
			r.port = jr->port;
			r.hostName = jr->host;
			results += r;
		}

		int id = i->id;
		resolveItemList.remove(i);
		emit resolve_resultsReady(id, results);
	}

	void jr_error()
	{
		JDnsServiceResolve *jr = (JDnsServiceResolve *)sender();
		ResolveItem *i = resolveItemList.itemByResolve(jr);
		Q_ASSERT(i);

		int id = i->id;
		resolveItemList.remove(i);
		emit resolve_error(id, ServiceResolver::ErrorGeneric);
	}

	void do_resolve_error(int id, XMPP::ServiceResolver::Error e)
	{
		ResolveItem *i = resolveItemList.itemById(id);
		Q_ASSERT(i);

		resolveItemList.remove(i);
		emit resolve_error(id, e);
	}

	void pub_addresses_hostName(const QByteArray &name)
	{
		printf("name=[%s]\n", name.data());
	}

		//connect(&jdns, SIGNAL(published(int)), SLOT(jdns_published(int)));

	/*virtual int publish_start(NameLocalPublisher::Mode pmode, const NameRecord &name)
	{
		if(mode == Unicast)
			return -1;

		QJDns::Response response;
		QJDns::Record record = exportJDNSRecord(name);
		response.answerRecords += record;
		QJDns::PublishMode m = (pmode == NameLocalPublisher::Unique ? QJDns::Unique : QJDns::Shared);
		return jdns.publishStart(m, record.owner, record.type, response);
	}

	virtual void publish_update(int id, const NameRecord &name)
	{
		QJDns::Response response;
		QJDns::Record record = exportJDNSRecord(name);
		response.answerRecords += record;
		return jdns.publishUpdate(id, response);
	}

	virtual void publish_stop(int id)
	{
		jdns.publishCancel(id);
	}*/

		//else if(e == QJDns::ErrorConflict)
		//	error = NameResolver::ErrorConflict;

		//if(mode == Multicast)
		//	jdns.queryCancel(id);
};

//----------------------------------------------------------------------------
// JDnsProvider
//----------------------------------------------------------------------------
class JDnsProvider : public IrisNetProvider
{
	Q_OBJECT
	Q_INTERFACES(XMPP::IrisNetProvider)

public:
	JDnsGlobal *global;

	JDnsProvider()
	{
		global = 0;
	}

	~JDnsProvider()
	{
		delete global;
	}

	void ensure_global()
	{
		if(!global)
			global = new JDnsGlobal;
	}

	virtual NameProvider *createNameProviderInternet()
	{
		ensure_global();
		return JDnsNameProvider::create(global, JDnsNameProvider::Internet);
	}

	virtual NameProvider *createNameProviderLocal()
	{
		ensure_global();
		return JDnsNameProvider::create(global, JDnsNameProvider::Local);
	}

	virtual ServiceProvider *createServiceProvider()
	{
		ensure_global();
		return JDnsServiceProvider::create(global);
	}
};

IrisNetProvider *irisnet_createJDnsProvider()
{
        return new JDnsProvider;
}

}

#include "netnames_jdns.moc"
