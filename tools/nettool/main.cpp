/*
 * Copyright (C) 2006  Justin Karneges
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include <stdio.h>
#include <iris/processquit.h>
#include <iris/netinterface.h>
#include <iris/netavailability.h>
#include <iris/netnames.h>
#include <iris/stunmessage.h>
#include <QtCrypto>

using namespace XMPP;

class NetMonitor : public QObject
{
	Q_OBJECT
public:
	NetInterfaceManager *man;
	QList<NetInterface*> ifaces;
	NetAvailability *netavail;

	~NetMonitor()
	{
		delete netavail;
		qDeleteAll(ifaces);
		delete man;
	}

signals:
	void quit();

public slots:
	void start()
	{
		connect(ProcessQuit::instance(), SIGNAL(quit()), SIGNAL(quit()));

		man = new NetInterfaceManager;
		connect(man, SIGNAL(interfaceAvailable(const QString &)),
			SLOT(here(const QString &)));
		QStringList list = man->interfaces();
		for(int n = 0; n < list.count(); ++n)
			here(list[n]);

		netavail = new NetAvailability;
		connect(netavail, SIGNAL(changed(bool)), SLOT(avail(bool)));
		avail(netavail->isAvailable());
	}

	void here(const QString &id)
	{
		NetInterface *iface = new NetInterface(id, man);
		connect(iface, SIGNAL(unavailable()), SLOT(gone()));
		printf("HERE: %s name=[%s]\n", qPrintable(iface->id()), qPrintable(iface->name()));
		QList<QHostAddress> addrs = iface->addresses();
		for(int n = 0; n < addrs.count(); ++n)
			printf("  address: %s\n", qPrintable(addrs[n].toString()));
		if(!iface->gateway().isNull())
			printf("  gateway: %s\n", qPrintable(iface->gateway().toString()));
		ifaces += iface;
	}

	void gone()
	{
		NetInterface *iface = (NetInterface *)sender();
		printf("GONE: %s\n", qPrintable(iface->id()));
		ifaces.removeAll(iface);
		delete iface;
	}

	void avail(bool available)
	{
		if(available)
			printf("** Network available\n");
		else
			printf("** Network unavailable\n");
	}
};

static QString dataToString(const QByteArray &buf)
{
	QString out;
	for(int n = 0; n < buf.size(); ++n)
	{
		unsigned char c = (unsigned char)buf[n];
		if(c == '\\')
			out += "\\\\";
		else if(c >= 0x20 || c < 0x7f)
			out += c;
		else
			out += QString("\\x%1").arg((uint)c, 2, 16);
	}
	return out;
}

static void print_record(const NameRecord &r)
{
	switch(r.type())
	{
		case NameRecord::A:
			printf("A: [%s] (ttl=%d)\n", qPrintable(r.address().toString()), r.ttl());
			break;
		case NameRecord::Aaaa:
			printf("AAAA: [%s] (ttl=%d)\n", qPrintable(r.address().toString()), r.ttl());
			break;
		case NameRecord::Mx:
			printf("MX: [%s] priority=%d (ttl=%d)\n", r.name().data(), r.priority(), r.ttl());
			break;
		case NameRecord::Srv:
			printf("SRV: [%s] port=%d priority=%d weight=%d (ttl=%d)\n", r.name().data(), r.port(), r.priority(), r.weight(), r.ttl());
			break;
		case NameRecord::Ptr:
			printf("PTR: [%s] (ttl=%d)\n", r.name().data(), r.ttl());
			break;
		case NameRecord::Txt:
		{
			QList<QByteArray> texts = r.texts();
			printf("TXT: count=%d (ttl=%d)\n", texts.count(), r.ttl());
			for(int n = 0; n < texts.count(); ++n)
				printf("  len=%d [%s]\n", texts[n].size(), qPrintable(dataToString(texts[n])));
			break;
		}
		case NameRecord::Hinfo:
			printf("HINFO: [%s] [%s] (ttl=%d)\n", r.cpu().data(), r.os().data(), r.ttl());
			break;
		case NameRecord::Null:
			printf("NULL: %d bytes (ttl=%d)\n", r.rawData().size(), r.ttl());
			break;
		default:
			printf("(Unknown): type=%d (ttl=%d)\n", r.type(), r.ttl());
			break;
	}
}

static int str2rtype(const QString &in)
{
	QString str = in.toLower();
	if(str == "a")
		return NameRecord::A;
	else if(str == "aaaa")
		return NameRecord::Aaaa;
	else if(str == "ptr")
		return NameRecord::Ptr;
	else if(str == "srv")
		return NameRecord::Srv;
	else if(str == "mx")
		return NameRecord::Mx;
	else if(str == "txt")
		return NameRecord::Txt;
	else if(str == "hinfo")
		return NameRecord::Hinfo;
	else if(str == "null")
		return NameRecord::Null;
	else
		return -1;
}

class ResolveName : public QObject
{
	Q_OBJECT
public:
	QString name;
	NameRecord::Type type;
	bool longlived;
	NameResolver dns;
	bool null_dump;

	ResolveName()
	{
		null_dump = false;
	}

public slots:
	void start()
	{
		connect(ProcessQuit::instance(), SIGNAL(quit()), SIGNAL(quit()));

		connect(&dns, SIGNAL(resultsReady(const QList<XMPP::NameRecord> &)),
			SLOT(dns_resultsReady(const QList<XMPP::NameRecord> &)));
		connect(&dns, SIGNAL(error(XMPP::NameResolver::Error)),
			SLOT(dns_error(XMPP::NameResolver::Error)));

		dns.start(name.toLatin1(), type, longlived ? NameResolver::LongLived : NameResolver::Single);
	}

signals:
	void quit();

private slots:
	void dns_resultsReady(const QList<XMPP::NameRecord> &list)
	{
		if(null_dump && list[0].type() == NameRecord::Null)
		{
			QByteArray buf = list[0].rawData();
			fwrite(buf.data(), buf.size(), 1, stdout);
		}
		else
		{
			for(int n = 0; n < list.count(); ++n)
				print_record(list[n]);
		}
		if(!longlived)
		{
			dns.stop();
			emit quit();
		}
	}

	void dns_error(XMPP::NameResolver::Error e)
	{
		QString str;
		if(e == NameResolver::ErrorNoName)
			str = "ErrorNoName";
		else if(e == NameResolver::ErrorTimeout)
			str = "ErrorTimeout";
		else if(e == NameResolver::ErrorNoLocal)
			str = "ErrorNoLocal";
		else if(e == NameResolver::ErrorNoLongLived)
			str = "ErrorNoLongLived";
		else // ErrorGeneric, or anything else
			str = "ErrorGeneric";

		printf("Error: %s\n", qPrintable(str));
		emit quit();
	}
};

class BrowseServices : public QObject
{
	Q_OBJECT
public:
	QString type, domain;
	ServiceBrowser browser;

public slots:
	void start()
	{
		connect(ProcessQuit::instance(), SIGNAL(quit()), SIGNAL(quit()));

		connect(&browser, SIGNAL(instanceAvailable(const XMPP::ServiceInstance &)),
			SLOT(browser_instanceAvailable(const XMPP::ServiceInstance &)));
		connect(&browser, SIGNAL(instanceUnavailable(const XMPP::ServiceInstance &)),
			SLOT(browser_instanceUnavailable(const XMPP::ServiceInstance &)));
		connect(&browser, SIGNAL(error()), SLOT(browser_error()));

		browser.start(type, domain);
	}

signals:
	void quit();

private slots:
	void browser_instanceAvailable(const XMPP::ServiceInstance &i)
	{
		printf("HERE: [%s] (%d attributes)\n", qPrintable(i.instance()), i.attributes().count());
		QMap<QString,QByteArray> attribs = i.attributes();
		QMapIterator<QString,QByteArray> it(attribs);
		while(it.hasNext())
		{
			it.next();
			printf("  [%s] = [%s]\n", qPrintable(it.key()), qPrintable(dataToString(it.value())));
		}
	}

	void browser_instanceUnavailable(const XMPP::ServiceInstance &i)
	{
		printf("GONE: [%s]\n", qPrintable(i.instance()));
	}

	void browser_error()
	{
	}
};

class ResolveService : public QObject
{
	Q_OBJECT
public:
	int mode;
	QString instance;
	QString type;
	QString domain;
	int port;

	ServiceResolver dns;

public slots:
	void start()
	{
		connect(ProcessQuit::instance(), SIGNAL(quit()), SIGNAL(quit()));

		connect(&dns, SIGNAL(resultsReady(const QHostAddress &, int)),
			SLOT(dns_resultsReady(const QHostAddress &, int)));
		connect(&dns, SIGNAL(finished()), SLOT(dns_finished()));
		connect(&dns, SIGNAL(error()), SLOT(dns_error()));

		if(mode == 0)
			dns.startFromInstance(instance.toLatin1() + '.' + type.toLatin1() + ".local.");
		else if(mode == 1)
			dns.startFromDomain(domain, type);
		else // 2
			dns.startFromPlain(domain, port);
	}

signals:
	void quit();

private slots:
	void dns_resultsReady(const QHostAddress &addr, int port)
	{
		printf("[%s] port=%d\n", qPrintable(addr.toString()), port);
		dns.tryNext();
	}

	void dns_finished()
	{
		emit quit();
	}

	void dns_error()
	{
	}
};

class PublishService : public QObject
{
	Q_OBJECT
public:
	QString instance;
	QString type;
	int port;
	QMap<QString,QByteArray> attribs;
	QByteArray extra_null;

	ServiceLocalPublisher pub;

public slots:
	void start()
	{
		//NetInterfaceManager::instance();

		connect(ProcessQuit::instance(), SIGNAL(quit()), SIGNAL(quit()));

		connect(&pub, SIGNAL(published()), SLOT(pub_published()));
		connect(&pub, SIGNAL(error(XMPP::ServiceLocalPublisher::Error)),
			SLOT(pub_error(XMPP::ServiceLocalPublisher::Error)));

		pub.publish(instance, type, port, attribs);
	}

signals:
	void quit();

private slots:
	void pub_published()
	{
		printf("Published\n");
		if(!extra_null.isEmpty())
		{
			NameRecord rec;
			rec.setNull(extra_null);
			pub.addRecord(rec);
		}
	}

	void pub_error(XMPP::ServiceLocalPublisher::Error e)
	{
		printf("Error: [%d]\n", e);
		emit quit();
	}
};

// pass valid magic and id pointers to do XOR-MAPPED-ADDRESS processing
// pass 0 for magic and id to do MAPPED-ADDRESS processing
static bool parse_mapped_address(const QByteArray &val, const quint8 *magic, const quint8 *id, QHostAddress *addr, quint16 *port)
{
	// val is at least 4 bytes
	if(val.size() < 4)
		return false;

	const quint8 *p = (const quint8 *)val.data();

	if(p[0] != 0)
		return false;

	quint16 _port;
	if(magic)
	{
		_port = p[2] ^ magic[0];
		_port <<= 8;
		_port += p[3] ^ magic[1];
	}
	else
	{
		_port = p[2];
		_port <<= 8;
		_port += p[3];
	}

	QHostAddress _addr;

	if(p[1] == 0x01)
	{
		// ipv4

		// val is 8 bytes in this case
		if(val.size() != 8)
			return false;

		quint32 addr4;
		if(magic)
		{
			addr4 = p[4] ^ magic[0];
			addr4 <<= 8;
			addr4 += p[5] ^ magic[1];
			addr4 <<= 8;
			addr4 += p[6] ^ magic[2];
			addr4 <<= 8;
			addr4 += p[7] ^ magic[3];
		}
		else
		{
			addr4 = p[4];
			addr4 <<= 8;
			addr4 += p[5];
			addr4 <<= 8;
			addr4 += p[6];
			addr4 <<= 8;
			addr4 += p[7];
		}
		_addr = QHostAddress(addr4);
	}
	else if(p[1] == 0x02)
	{
		// ipv6

		// val is 20 bytes in this case
		if(val.size() != 20)
			return false;

		quint8 tmp[16];
		for(int n = 0; n < 16; ++n)
		{
			quint8 x;
			if(n < 4)
				x = magic[n];
			else
				x = id[n - 4];

			tmp[n] = p[n + 4] ^ x;
		}

		_addr = QHostAddress(tmp);
	}
	else
		return false;

	*addr = _addr;
	*port = _port;
	return true;
}

class StunBinding : public QObject
{
	Q_OBJECT
public:
	QHostAddress addr;
	int port;
	int localPort;
	QUdpSocket *sock;
	QTimer *t;
	int rto, rc, rm;
	int tries;
	int last_interval;
	StunMessage outMessage;
	QByteArray packet;

public slots:
	void start()
	{
		sock = new QUdpSocket(this);
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));

		t = new QTimer(this);
		connect(t, SIGNAL(timeout()), SLOT(t_timeout()));
		t->setSingleShot(true);

		if(!sock->bind(localPort != -1 ? localPort : 0))
		{
			printf("Error binding to local port.\n");
			emit quit();
			return;
		}

		printf("Bound to local port %d.\n", sock->localPort());

		QByteArray id = QCA::Random::randomArray(12).toByteArray();

		outMessage.setClass(StunMessage::Request);
		outMessage.setMethod(0x001);
		outMessage.setId((const quint8 *)id.data());

		packet = outMessage.toBinary();
		if(packet.isEmpty())
		{
			printf("Error serializing STUN message.\n");
			emit quit();
			return;
		}

		// default RTO/Rc/Rm values from RFC 5389
		rto = 500;
		rc = 7;
		rm = 16;
		tries = 0;
		last_interval = rm * rto;
		trySend();
	}

signals:
	void quit();

private slots:
	void sock_readyRead()
	{
		QByteArray buf(sock->pendingDatagramSize(), 0);
		QHostAddress from;
		quint16 fromPort;

		sock->readDatagram(buf.data(), buf.size(), &from, &fromPort);
		if(from == addr && fromPort == port)
		{
			StunMessage message = StunMessage::fromBinary(buf);
			if(message.isNull())
			{
				printf("Server responded with what doesn't seem to be a STUN packet, skipping.\n");
				return;
			}

			if(message.mclass() != StunMessage::SuccessResponse && message.mclass() != StunMessage::ErrorResponse)
			{
				printf("Error: received unexpected message class type, skipping.\n");
				return;
			}

			if(memcmp(message.id(), outMessage.id(), 12) != 0)
			{
				printf("Error: received unexpected transaction id, skipping.\n");
				return;
			}

			if(message.mclass() == StunMessage::ErrorResponse)
			{
				printf("Error: server responded with an error.\n");
				t->stop();
				emit quit();
				return;
			}

			QHostAddress saddr;
			quint16 sport = 0;

			QByteArray val;
			val = message.attribute(0x0020);
			if(!val.isNull())
			{
				if(!parse_mapped_address(val, message.magic(), message.id(), &saddr, &sport))
				{
					printf("Error parsing XOR-MAPPED-ADDRESS response.\n");
					t->stop();
					emit quit();
					return;
				}
			}
			else
			{
				val = message.attribute(0x0001);
				if(!val.isNull())
				{
					if(!parse_mapped_address(val, 0, 0, &saddr, &sport))
					{
						printf("Error parsing MAPPED-ADDRESS response.\n");
						t->stop();
						emit quit();
						return;
					}
				}
				else
				{
					printf("Error: response does not contain XOR-MAPPED-ADDRESS or MAPPED-ADDRESS.\n");
					t->stop();
					emit quit();
					return;
				}
			}

			printf("Server says we are %s;%d\n", qPrintable(saddr.toString()), sport);
			t->stop();
			emit quit();
		}
		else
		{
			printf("Response from unknown sender %s:%d, dropping.\n", qPrintable(from.toString()), fromPort);
		}
	}

	void t_timeout()
	{
		if(tries == rc)
		{
			printf("Error: Timeout\n");
			t->stop();
			emit quit();
			return;
		}

		trySend();
	}

	void trySend()
	{
		++tries;
		sock->writeDatagram(packet, addr, port);

		if(tries == rc)
		{
			t->start(last_interval);
		}
		else
		{
			t->start(rto);
			rto *= 2;
		}
	}
};

void usage()
{
	printf("nettool: simple testing utility\n");
	printf("usage: nettool [command]\n");
	printf("\n");
	printf(" netmon                                          monitor network interfaces\n");
	printf(" rname (-r) [domain] (record type)               look up record (default = a)\n");
	printf(" rnamel [domain] [record type]                   look up record (long-lived)\n");
	printf(" browse [service type]                           browse for local services\n");
	printf(" rservi [instance] [service type]                look up browsed instance\n");
	printf(" rservd [domain] [service type]                  look up normal SRV\n");
	printf(" rservp [domain] [port]                          look up non-SRV\n");
	printf(" pserv [inst] [type] [port] (attr) (-a [rec])    publish service instance\n");
	printf(" stun [addr](;port) (local port)                 STUN binding\n");
	printf("\n");
	printf("record types: a aaaa ptr srv mx txt hinfo null\n");
	printf("service types: _service._proto format (e.g. \"_xmpp-client._tcp\")\n");
	printf("attributes: var0[=val0],...,varn[=valn]\n");
	printf("rname -r: for null type, dump raw record data to stdout\n");
	printf("pub -a: add extra record.  format: null:filename.dat\n");
	printf("\n");
}

int main(int argc, char **argv)
{
	QCA::Initializer qcaInit;
	QCoreApplication app(argc, argv);
	if(argc < 2)
	{
		usage();
		return 1;
	}

	QStringList args;
	for(int n = 1; n < argc; ++n)
		args += argv[n];

	if(args[0] == "netmon")
	{
		NetMonitor a;
		QObject::connect(&a, SIGNAL(quit()), &app, SLOT(quit()));
		QTimer::singleShot(0, &a, SLOT(start()));
		app.exec();
	}
	else if(args[0] == "rname" || args[0] == "rnamel")
	{
		bool null_dump = false;
		for(int n = 1; n < args.count(); ++n)
		{
			if(args[n] == "-r")
			{
				null_dump = true;
				args.removeAt(n);
				--n;
			}
		}

		if(args.count() < 2)
		{
			usage();
			return 1;
		}
		if(args[0] == "rnamel" && args.count() < 3)
		{
			usage();
			return 1;
		}
		int x = NameRecord::A;
		if(args.count() >= 3)
		{
			x = str2rtype(args[2]);
			if(x == -1)
			{
				usage();
				return 1;
			}
		}
		ResolveName a;
		a.name = args[1];
		a.type = (NameRecord::Type)x;
		a.longlived = (args[0] == "rnamel") ? true : false;
		if(args[0] == "rname" && null_dump)
			a.null_dump = true;
		QObject::connect(&a, SIGNAL(quit()), &app, SLOT(quit()));
		QTimer::singleShot(0, &a, SLOT(start()));
		app.exec();
	}
	else if(args[0] == "browse")
	{
		if(args.count() < 2)
		{
			usage();
			return 1;
		}

		BrowseServices a;
		a.type = args[1];
		QObject::connect(&a, SIGNAL(quit()), &app, SLOT(quit()));
		QTimer::singleShot(0, &a, SLOT(start()));
		app.exec();
	}
	else if(args[0] == "rservi" || args[0] == "rservd" || args[0] == "rservp")
	{
		// they all take 2 params
		if(args.count() < 3)
		{
			usage();
			return 1;
		}

		ResolveService a;
		if(args[0] == "rservi")
		{
			a.mode = 0;
			a.instance = args[1];
			a.type = args[2];
		}
		else if(args[0] == "rservd")
		{
			a.mode = 1;
			a.domain = args[1];
			a.type = args[2];
		}
		else // rservp
		{
			a.mode = 2;
			a.domain = args[1];
			a.port = args[2].toInt();
		}
		QObject::connect(&a, SIGNAL(quit()), &app, SLOT(quit()));
		QTimer::singleShot(0, &a, SLOT(start()));
		app.exec();
	}
	else if(args[0] == "pserv")
	{
		QStringList addrecs;
		for(int n = 1; n < args.count(); ++n)
		{
			if(args[n] == "-a")
			{
				if(n + 1 < args.count())
				{
					addrecs += args[n + 1];
					args.removeAt(n);
					args.removeAt(n);
					--n;
				}
				else
				{
					usage();
					return 1;
				}
			}
		}

		QByteArray extra_null;
		for(int n = 0; n < addrecs.count(); ++n)
		{
			const QString &str = addrecs[n];
			int x = str.indexOf(':');
			if(x == -1 || str.mid(0, x) != "null")
			{
				usage();
				return 1;
			}

			QString null_file = str.mid(x + 1);

			if(!null_file.isEmpty())
			{
				QFile f(null_file);
				if(!f.open(QFile::ReadOnly))
				{
					printf("can't read file\n");
					return 1;
				}
				extra_null = f.readAll();
			}
		}

		if(args.count() < 4)
		{
			usage();
			return 1;
		}

		QMap<QString,QByteArray> attribs;
		if(args.count() > 4)
		{
			QStringList parts = args[4].split(',');
			for(int n = 0; n < parts.count(); ++n)
			{
				const QString &str = parts[n];
				int x = str.indexOf('=');
				if(x != -1)
					attribs.insert(str.mid(0, x), str.mid(x + 1).toUtf8());
				else
					attribs.insert(str, QByteArray());
			}
		}

		PublishService a;
		a.instance = args[1];
		a.type = args[2];
		a.port = args[3].toInt();
		a.attribs = attribs;
		a.extra_null = extra_null;
		QObject::connect(&a, SIGNAL(quit()), &app, SLOT(quit()));
		QTimer::singleShot(0, &a, SLOT(start()));
		app.exec();
	}
	else if(args[0] == "stun")
	{
		if(args.count() < 2)
		{
			usage();
			return 1;
		}

		QString addrstr, portstr;
		int x = args[1].indexOf(';');
		if(x != -1)
		{
			addrstr = args[1].mid(0, x);
			portstr = args[1].mid(x + 1);
		}
		else
			addrstr = args[1];

		QHostAddress addr = QHostAddress(addrstr);
		if(addr.isNull())
		{
			printf("Error: addr must be an IP address\n");
			return 1;
		}

		int port = 3478;
		if(!portstr.isEmpty())
			port = portstr.toInt();

		int localPort = -1;
		if(args.count() >= 3)
			localPort = args[2].toInt();

		StunBinding a;
		a.localPort = localPort;
		a.addr = addr;
		a.port = port;
		QObject::connect(&a, SIGNAL(quit()), &app, SLOT(quit()));
		QTimer::singleShot(0, &a, SLOT(start()));
		app.exec();
	}
	else
	{
		usage();
		return 1;
	}
	return 0;
}

#include "main.moc"
