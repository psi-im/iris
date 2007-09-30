#include <QtCore>
#include "qdnssd.h"

class Command
{
public:
	enum Type
	{
		Query,
		Browse,
		Resolve,
		Reg
	};

	Type type;
	QString name;
	int rtype;
	QString stype;
	QString domain;
	int port;
	QStringList txt;

	int id;
	int dnsId;
};

static QString nameToString(const QByteArray &in)
{
	QStringList parts;
	int at = 0;
	while(at < in.size())
	{
		int len = in[at++];
		parts += QString::fromLatin1(in.mid(at, len));
		at += len;
	}
	return parts.join(".");
}

class App : public QObject
{
	Q_OBJECT
public:
	QList<Command> commands;
	QDnsSd *dns;

	App()
	{
		dns = new QDnsSd(this);
		connect(dns, SIGNAL(queryResult(int, const QDnsSd::QueryResult &)), SLOT(dns_queryResult(int, const QDnsSd::QueryResult &)));
		connect(dns, SIGNAL(browseResult(int, const QDnsSd::BrowseResult &)), SLOT(dns_browseResult(int, const QDnsSd::BrowseResult &)));
		connect(dns, SIGNAL(resolveResult(int, const QDnsSd::ResolveResult &)), SLOT(dns_resolveResult(int, const QDnsSd::ResolveResult &)));
		connect(dns, SIGNAL(regResult(int, const QDnsSd::RegResult &)), SLOT(dns_regResult(int, const QDnsSd::RegResult &)));
	}

public slots:
	void start()
	{
		for(int n = 0; n < commands.count(); ++n)
		{
			Command &c = commands[n];

			c.id = n;
			if(c.type == Command::Query)
			{
				c.dnsId = dns->query(c.name.toLatin1(), c.rtype);
			}
			else if(c.type == Command::Browse)
			{
				c.dnsId = dns->browse(c.name.toLatin1(), c.domain.toLatin1());
			}
			else if(c.type == Command::Resolve)
			{
				c.dnsId = dns->resolve(c.name.toLatin1(), c.stype.toLatin1(), c.domain.toLatin1());
			}
			else if(c.type == Command::Reg)
			{
				QByteArray txtRecord;

				// TODO: calculate txtRecord from c.txt
				c.dnsId = dns->reg(c.name.toLatin1(), c.stype.toLatin1(), c.domain.toLatin1(), c.port, txtRecord);
			}
		}
	}

signals:
	void quit();

private:
	int dnsIdToCommandIndex(int dnsId)
	{
		for(int n = 0; n < commands.count(); ++n)
		{
			const Command &c = commands[n];
			if(c.dnsId == dnsId)
				return n;
		}
		return -1;
	}

private slots:
	void dns_queryResult(int id, const QDnsSd::QueryResult &result)
	{
		int at = dnsIdToCommandIndex(id);
		Command &c = commands[at];

		if(!result.success)
		{
			printf("%2d: error.\n", c.id);
			return;
		}

		QString desc;
		if(rec.rrtype == 1)
		{
			quint32 *p = (quint32 *)rec.rdata.data();
			desc = QHostAddress(*p).toString();
		}
		else if(rec.rrtype == 28)
		{
			desc = QHostAddress((quint8 *)rec.rdata.data()).toString();
		}
		else if(rec.rrtype == 12)
		{
			desc = QString("[%1]").arg(nameToString(rec.rdata));
		}
		else
			desc = QString("%1 bytes").arg(rec.rdata.size());

		foreach(const QDnsSd::Record &rec, result.added)
			printf("%2d: added:   %s, ttl=%d\n", c.id, qPrintable(desc), rec.ttl);
		foreach(const QDnsSd::Record &rec, result.removed)
			printf("%2d: removed: %s, ttl=%d\n", c.id, qPrintable(desc), rec.ttl);
	}

	void dns_browseResult(int id, const QDnsSd::BrowseResult &result)
	{
		int at = dnsIdToCommandIndex(id);
		Command &c = commands[at];

		if(!result.success)
		{
			printf("%2d: error.\n", c.id);
			return;
		}

		foreach(const QDnsSd::BrowseEntry &e, result.added)
			printf("%2d: added:   [%s] [%s] [%s]\n", c.id, e.serviceName.data(), e.serviceType.data(), e.replyDomain.data());
		foreach(const QDnsSd::BrowseEntry &e, result.removed)
			printf("%2d: removed: [%s]\n", c.id, e.serviceName.data());
	}

	void dns_resolveResult(int id, const QDnsSd::ResolveResult &result)
	{
		int at = dnsIdToCommandIndex(id);
		Command &c = commands[at];

		if(!result.success)
		{
			printf("%2d: error.\n", c.id);
			return;
		}

		// TODO: print out the txt
		printf("%2d: host=[%s] port=%d (%d bytes txt)\n", c.id, result.hostTarget.data(), result.port, result.txtRecord.size());
	}

	void dns_regResult(int id, const QDnsSd::RegResult &result)
	{
		int at = dnsIdToCommandIndex(id);
		Command &c = commands[at];

		if(!result.success)
		{
			QString errstr;
			if(c.errorCode == QDnsSd::RegResult::ErrorConflict)
				errstr = "conflict";
			else
				errstr = "generic";
			printf("%2d: error (%s).\n", c.id, qPrintable(errstr));
			return;
		}

		printf("%2d: registered.  domain=[%s]\n", c.id, result.domain.data());
	}
};

#include "main.moc"

void usage()
{
	printf("usage: sdtest [[command] (command) ...]\n");
	printf(" options: --txt=str0,...,strn\n");
	printf("\n");
	printf(" q=name,type#                   query for a record\n");
	printf(" b=type(,domain)                browse for a service type\n");
	printf(" r=name,type(,domain)           resolve a service\n");
	printf(" e=name,type,port(,domain)      register a service\n");
	printf("\n");
}

int main(int argc, char **argv)
{
	QCoreApplication qapp(argc, argv);

	QStringList args = qapp.arguments();
	args.removeFirst();

	if(args.count() < 1)
	{
		usage();
		return 1;
	}

	// options
	QStringList txt;

	for(int n = 0; n < args.count(); ++n)
	{
		QString s = args[n];
		if(!s.startsWith("--"))
			continue;
		QString var;
		QString val;
		int x = s.indexOf('=');
		if(x != -1)
		{
			var = s.mid(2, x - 2);
			val = s.mid(x + 1);
		}
		else
		{
			var = s.mid(2);
		}

		bool known = true;

		if(var == "txt")
		{
			txt = val.split(',');
		}
		else
			known = false;

		if(known)
		{
			args.removeAt(n);
			--n; // adjust position
		}
	}

	// commands
	QList<Command> commands;

	for(int n = 0; n < args.count(); ++n)
	{
		QString str = args[n];
		int n = str.indexOf('=');
		if(n == -1)
		{
			printf("Error: bad format of command.\n");
			return 1;
		}

		QString type = str.mid(0, n);
		QString rest = str.mid(n + 1);
		QStringList parts = rest.split(',');

		if(type == "q")
		{
			if(parts.count() < 2)
			{
				usage();
				return 1;
			}

			Command c;
			c.type = Command::Query;
			c.name = parts[0];
			c.rtype = parts[1].toInt();
			commands += c;
		}
		else if(type == "b")
		{
			if(parts.count() < 1)
			{
				usage();
				return 1;
			}

			Command c;
			c.type = Command::Browse;
			c.stype = parts[0];
			if(parts.count() >= 2)
				c.domain = parts[1];
			commands += c;
		}
		else if(type == "r")
		{
			if(parts.count() < 2)
			{
				usage();
				return 1;
			}

			Command c;
			c.type = Command::Resolve;
			c.name = parts[0];
			c.stype = parts[1];
			if(parts.count() >= 3)
				c.domain = parts[2];
			commands += c;
		}
		else if(type == "e")
		{
			if(parts.count() < 3)
			{
				usage();
				return 1;
			}

			Command c;
			c.type = Command::Reg;
			c.name = parts[0];
			c.stype = parts[1];
			c.port = parts[2].toInt();
			if(parts.count() >= 4)
				c.domain = parts[3];
			c.txt = txt;
			commands += c;
		}
		else
		{
			printf("Error: unknown command type '%s'.\n", qPrintable(type));
			return 1;
		}
	}

	App app;
	app.commands = commands;
	QObject::connect(&app, SIGNAL(quit()), &qapp, SLOT(quit()));
	QTimer::singleShot(0, &app, SLOT(start()));
	qapp.exec();
	return 0;
}
