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

Q_DECLARE_METATYPE(XMPP::NameRecord)
Q_DECLARE_METATYPE(XMPP::NameResolver::Error)

namespace XMPP {

NameRecord importJDNSRecord(const QJDns::Record &in)
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

QJDns::Record exportJDNSRecord(const NameRecord &in)
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
		// TODO
		QStringList lines = db.readDebugLines();
		Q_UNUSED(lines);
		//for(int n = 0; n < lines.count(); ++n)
		//	printf("jdns: %s\n", qPrintable(lines[n]));
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
		if(!i)
			return;

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
				out += importJDNSRecord(results[n]);
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
// JDnsServiceProvider
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

class JDnsBrowseInfo
{
public:
	QByteArray name;
	QByteArray instance;
	QByteArray srvhost;
	int srvport;
	QList<QByteArray> attribs;
};

class JDnsBrowse : public QObject
{
	Q_OBJECT

public:
	int id;

	JDnsShared *jdns;
	JDnsSharedRequest *req;
	QByteArray type;

	class Lookup
	{
	public:
		QByteArray instance;
		QByteArray name;
		JDnsServiceResolve *resolve;
	};
	QList<Lookup> lookups;

	JDnsBrowse(JDnsShared *_jdns)
	{
		req = 0;
		jdns = _jdns;
	}

	~JDnsBrowse()
	{
		foreach(const Lookup &lu, lookups)
			delete lu.resolve;
		//qDeleteAll(lookups);
		delete req;
	}

	void start(const QByteArray &_type)
	{
		type = _type;
		req = new JDnsSharedRequest(jdns);
		connect(req, SIGNAL(resultsReady()), SLOT(jdns_resultsReady()));
		req->query(type + ".local.", QJDns::Ptr);
	}

signals:
	void available(const JDnsBrowseInfo &i);
	void unavailable(const QByteArray &instance);

private slots:
	void jdns_resultsReady()
	{
		QJDns::Record rec = req->results().first();
		QByteArray name = rec.name;

		// FIXME: this is wrong, it should search backwards
		int x = name.indexOf('.');
		QByteArray instance = name.mid(0, x);

		if(rec.ttl == 0)
		{
			// stop any lookups
			JDnsServiceResolve *bl = 0;
			int at = -1;
			for(int n = 0; n < lookups.count(); ++n)
			{
				if(lookups[n].name == name)
				{
					bl = lookups[n].resolve;
					at = n;
					break;
				}
			}
			if(bl)
			{
				//lookups.removeAll(bl);
				lookups.removeAt(at);
				delete bl;
			}

			//printf("Instance Gone: [%s]\n", instance.data());
			emit unavailable(instance);
			return;
		}

		//printf("Instance Found: [%s]\n", instance.data());

		//printf("Lookup starting\n");
		/*JDnsServiceResolve *bl = new JDnsServiceResolve(jdns);
		connect(bl, SIGNAL(finished()), SLOT(bl_finished()));
		Lookup lu;
		lu.instance = instance;
		lu.resolve = bl;
		lookups += lu;
		bl->start(name);*/

		JDnsBrowseInfo i;
		i.name = name;
		i.instance = instance;
		//i.srvhost = bl->srvhost;
		//i.srvport = bl->srvport;
		//i.attribs = bl->attribs;
		emit available(i);
	}

	void bl_finished()
	{
		JDnsServiceResolve *bl = (JDnsServiceResolve *)sender();

		int at = -1;
		for(int n = 0; n < lookups.count(); ++n)
		{
			if(lookups[n].resolve == bl)
			{
				at = n;
				break;
			}
		}
		if(at == -1)
			return;

		JDnsBrowseInfo i;
		i.name = lookups[at].name;
		i.instance = lookups[at].instance;
		//i.srvhost = bl->srvhost;
		//i.srvport = bl->srvport;
		i.attribs = bl->attribs;

		lookups.removeAt(at);
		delete bl;

		//printf("Lookup finished\n");
		emit available(i);
	}
};

class JDnsServiceProvider : public ServiceProvider
{
	Q_OBJECT

public:
	JDnsGlobal *global;

	QList<JDnsBrowse*> list;
	QHash<QByteArray,ServiceInstance> items;

	QList<JDnsSharedRequest*> pubitems;
	QByteArray _servname;

	static JDnsServiceProvider *create(JDnsGlobal *global, QObject *parent = 0)
	{
		JDnsServiceProvider *p = new JDnsServiceProvider(global, parent);
		return p;
	}

	JDnsServiceProvider(JDnsGlobal *_global, QObject *parent = 0) :
		ServiceProvider(parent)
	{
		global = _global;
	}

	~JDnsServiceProvider()
	{
		qDeleteAll(pubitems);
	}

	virtual int browse_start(const QString &type, const QString &domain)
	{
		// no support for non-local domains
		if(!domain.isEmpty() && (domain != ".local." && domain != ".local" && domain != "."))
		{
			// TODO
		}

		if(!global->ensure_mul())
		{
			// TODO
		}

		JDnsBrowse *b = new JDnsBrowse(global->mul);
		connect(b, SIGNAL(available(const JDnsBrowseInfo &)), SLOT(jb_available(const JDnsBrowseInfo &)));
		connect(b, SIGNAL(unavailable(const QByteArray &)), SLOT(jb_unavailable(const QByteArray &)));
		b->start(type.toLatin1());

		return 1;
	}

	virtual void browse_stop(int id)
	{
		// TODO
		Q_UNUSED(id);
	}

	virtual int resolve_start(const QByteArray &name)
	{
		if(!global->ensure_mul())
		{
			// TODO
		}

		JDnsServiceResolve *bl = new JDnsServiceResolve(global->mul);
		connect(bl, SIGNAL(finished()), SLOT(bl_finished()));
		connect(bl, SIGNAL(error()), SLOT(bl_error()));
		bl->start(name);

		return 1;
	}

	virtual void resolve_stop(int id)
	{
		// TODO
		Q_UNUSED(id);
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
	void jb_available(const JDnsBrowseInfo &i)
	{
		//printf("jb_available: [%s]\n", i.instance.data());
		JDnsBrowse *b = (JDnsBrowse *)sender();
		QMap<QString,QByteArray> map;
		for(int n = 0; n < i.attribs.count(); ++n)
		{
			const QByteArray &a = i.attribs[n];
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

			map.insert(key, value);
		}
		ServiceInstance si(QString::fromLatin1(i.instance), QString::fromLatin1(b->type), "local.", map);
		items.insert(i.name, si);
		emit browse_instanceAvailable(1, si);
	}

	void jb_unavailable(const QByteArray &instance)
	{
		//printf("jb_unavailable: [%s]\n", instance.data());
		JDnsBrowse *b = (JDnsBrowse *)sender();
		QByteArray name = instance + '.' + b->type + ".local.";
		if(!items.contains(name))
			return;

		ServiceInstance si = items.value(name);
		items.remove(name);
		emit browse_instanceUnavailable(1, si);
	}

	void bl_finished()
	{
		JDnsServiceResolve *bl = (JDnsServiceResolve *)sender();
		QMap<QString,QByteArray> attribs;
		for(int n = 0; n < bl->attribs.count(); ++n)
		{
			const QByteArray &a = bl->attribs[n];
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

		QByteArray host = bl->host;
		bool have6 = bl->have6;
		bool have4 = bl->have4;
		QHostAddress addr6 = bl->addr6;
		QHostAddress addr4 = bl->addr4;
		int port = bl->port;
		delete bl;

		// one of these must be true
		Q_ASSERT(have4 || have6);

		QList<ResolveResult> results;
		if(have6)
		{
			ResolveResult r;
			r.attributes = attribs;
			r.address = addr6;
			r.port = port;
			r.hostName = host;
			results += r;
		}
		if(have4)
		{
			ResolveResult r;
			r.attributes = attribs;
			r.address = addr4;
			r.port = port;
			r.hostName = host;
			results += r;
		}

		emit resolve_resultsReady(1, results);
	}

	void bl_error()
	{
		printf("resolve error\n");
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
