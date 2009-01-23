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

#include <QCoreApplication>
#include <QTimer>
#include <QtCrypto>
#include <iris/ice176.h>
#include <stdio.h>

// some encoding routines taken from psimedia demo
static QString urlishEncode(const QString &in)
{
	QString out;
	for(int n = 0; n < in.length(); ++n)
	{
		if(in[n] == '%' || in[n] == ',' || in[n] == ';' || in[n] == ':' || in[n] == '\n')
		{
			unsigned char c = (unsigned char)in[n].toLatin1();
			out += QString().sprintf("%%%02x", c);
		}
		else
			out += in[n];
	}
	return out;
}

static QString urlishDecode(const QString &in)
{
	QString out;
	for(int n = 0; n < in.length(); ++n)
	{
		if(in[n] == '%')
		{
			if(n + 2 >= in.length())
				return QString();

			QString hex = in.mid(n + 1, 2);
			bool ok;
			int x = hex.toInt(&ok, 16);
			if(!ok)
				return QString();

			unsigned char c = (unsigned char)x;
			out += c;
			n += 2;
		}
		else
			out += in[n];
	}
	return out;
}

static QString candidate_to_line(const XMPP::Ice176::Candidate &in)
{
	QStringList list;
	list += in.component;
	list += in.foundation;
	list += QString::number(in.generation);
	list += in.id;
	list += in.ip.toString();
	list += QString::number(in.network);
	list += QString::number(in.port);
	list += QString::number(in.priority);
	list += in.protocol;
	list += in.rel_addr.toString();
	list += QString::number(in.rel_port);
	list += in.rem_addr.toString();
	list += QString::number(in.rem_port);
	list += in.type;

	for(int n = 0; n < list.count(); ++n)
		list[n] = urlishEncode(list[n]);
	return list.join(",");
}

static XMPP::Ice176::Candidate line_to_candidate(const QString &in)
{
	QStringList list = in.split(',');
	if(list.count() < 14)
		return XMPP::Ice176::Candidate();

	for(int n = 0; n < list.count(); ++n)
	{
		QString str = urlishDecode(list[n]);
		if(str.isEmpty())
			return XMPP::Ice176::Candidate();
		list[n] = str;
	}

	XMPP::Ice176::Candidate out;
	out.component = list[0];
	out.foundation = list[1];
	out.generation = list[2].toInt();
	out.id = list[3];
	out.ip = QHostAddress(list[4]);
	out.network = list[5].toInt();
	out.port = list[6].toInt();
	out.priority = list[7].toInt();
	out.protocol = list[8];
	out.rel_addr = QHostAddress(list[9]);
	out.rel_port = list[10].toInt();
	out.rem_addr = QHostAddress(list[11]);
	out.rem_port = list[12].toInt();
	out.type = list[13];
	return out;
}

static QStringList iceblock_create(const QList<XMPP::Ice176::Candidate> &in)
{
	QStringList out;
	out += "-----BEGIN ICE-----";
	foreach(const XMPP::Ice176::Candidate &c, in)
		out += candidate_to_line(c);
	out += "-----END ICE-----";
	return out;
}

static QList<XMPP::Ice176::Candidate> iceblock_parse(const QStringList &in)
{
	QList<XMPP::Ice176::Candidate> out;
	if(in.count() < 3 || in[0] != "-----BEGIN ICE-----" || in[in.count()-1] != "-----END ICE-----")
		return out;
	for(int n = 1; n < in.count() - 1; ++n)
	{
		XMPP::Ice176::Candidate c = line_to_candidate(in[n]);
		if(c.type.isEmpty())
			return QList<XMPP::Ice176::Candidate>();
		out += c;
	}
	return out;
}

static QStringList iceblock_read()
{
	QStringList out;

	FILE *fp = stdin;
	while(1)
	{
		QByteArray line(1024, 0);
		fgets(line.data(), line.size(), fp);
		if(feof(fp))
			break;

		// hack off newline
		line.resize(line.size() - 1);

		QString str = QString::fromLocal8Bit(line);
		out += str;
		if(str == "-----END ICE-----")
			break;
	}

	return out;
}

class App : public QObject
{
	Q_OBJECT

public:
	int opt_mode;
	int opt_localBase;
	int opt_channels;
	QString opt_stunHost;
	bool opt_is_relay;
	QString opt_user, opt_pass;

	App()
	{
	}

public slots:
	void start()
	{
		emit quit();
	}

signals:
	void quit();
};

void usage()
{
	printf("icetunnel: create a peer-to-peer UDP tunnel based on ICE\n");
	printf("usage: icetunnel initiator (options)\n");
	printf("       icetunnel responder (options)\n");
	printf("\n");
	printf(" --localbase=[n]     local base port (default=60000)\n");
	printf(" --channels=[n]      number of channels to create (default=4)\n");
	printf(" --stun=[host]       STUN server to use\n");
	printf(" --relay             set if STUN server supports relaying (TURN)\n");
	printf(" --user=[user]       STUN server username\n");
	printf(" --pass=[pass]       STUN server password\n");
	printf("\n");
}

int main(int argc, char **argv)
{
	QCA::Initializer qcaInit;
	QCoreApplication qapp(argc, argv);

	QStringList args = qapp.arguments();
	args.removeFirst();

	int localBase = 60000;
	int channels = 4;
	QString stunHost;
	bool is_relay = false;
	QString user, pass;

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

		if(var == "localbase")
			localBase = val.toInt();
		else if(var == "channels")
			channels = val.toInt();
		else if(var == "stun")
			stunHost = val;
		else if(var == "relay")
			is_relay = true;
		else if(var == "user")
			user = val;
		else if(var == "pass")
			pass = val;
		else
			known = false;

		if(!known)
		{
			fprintf(stderr, "Unknown option '%s'.\n", qPrintable(var));
			return 1;
		}

		args.removeAt(n);
		--n; // adjust position
	}

	if(args.isEmpty())
	{
		usage();
		return 1;
	}

	int mode = -1;
	if(args[0] == "initiator")
		mode = 0;
	else if(args[0] == "responder")
		mode = 1;

	if(mode == -1)
	{
		usage();
		return 1;
	}

	App app;
	app.opt_mode = mode;
	app.opt_localBase = localBase;
	app.opt_channels = channels;
	app.opt_stunHost = stunHost;
	app.opt_is_relay = is_relay;
	app.opt_user = user;
	app.opt_pass = pass;

	QObject::connect(&app, SIGNAL(quit()), &qapp, SLOT(quit()));
	QTimer::singleShot(0, &app, SLOT(start()));
	qapp.exec();

	return 0;
}

#include "main.moc"
