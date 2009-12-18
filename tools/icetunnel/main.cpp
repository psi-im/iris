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
#include <QUdpSocket>
#include <QNetworkInterface>
#include <QtCrypto>
#include <iris/netnames.h>
#include <iris/netinterface.h>
#include <iris/ice176.h>
#include <stdio.h>

// some encoding routines taken from psimedia demo
static QString urlishEncode(const QString &in)
{
	QString out;
	for(int n = 0; n < in.length(); ++n)
	{
		if(in[n] == '%' || in[n] == ',' || in[n] == ';' || in[n] == ':' || in[n] == ' ' || in[n] == '\n')
		{
			unsigned char c = (unsigned char)in[n].toLatin1();
			out += QString().sprintf("%%%02x", c);
		}
		else
			out += in[n];
	}
	return out;
}

static QString urlishDecode(const QString &in, bool *ok = 0)
{
	QString out;
	for(int n = 0; n < in.length(); ++n)
	{
		if(in[n] == '%')
		{
			if(n + 2 >= in.length())
			{
				if(ok)
					*ok = false;
				return QString();
			}

			QString hex = in.mid(n + 1, 2);
			bool b;
			int x = hex.toInt(&b, 16);
			if(!b)
			{
				if(ok)
					*ok = false;
				return QString();
			}

			unsigned char c = (unsigned char)x;
			out += c;
			n += 2;
		}
		else
			out += in[n];
	}

	if(ok)
		*ok = true;
	return out;
}

static QString candidate_to_line(const XMPP::Ice176::Candidate &in)
{
	QStringList list;
	list += QString::number(in.component);
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
		bool ok;
		QString str = urlishDecode(list[n], &ok);
		if(!ok)
			return XMPP::Ice176::Candidate();
		list[n] = str;
	}

	XMPP::Ice176::Candidate out;
	out.component = list[0].toInt();
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

class IceOffer
{
public:
	QString user, pass;
	QList<XMPP::Ice176::Candidate> candidates;
};

static QStringList line_wrap(const QString &in, int maxlen)
{
	Q_ASSERT(maxlen >= 1);

	QStringList out;
	int at = 0;
	while(at < in.length())
	{
		int takeAmount = qMin(maxlen, in.length() - at);
		out += in.mid(at, takeAmount);
		at += takeAmount;
	}
	return out;
}

static QString lines_unwrap(const QStringList &in)
{
	return in.join(QString());
}

static QStringList iceblock_create(const IceOffer &in)
{
	QStringList out;
	out += "-----BEGIN ICE-----";
	{
		QStringList body;
		QStringList userpass;
		userpass += urlishEncode(in.user);
		userpass += urlishEncode(in.pass);
		body += userpass.join(",");
		foreach(const XMPP::Ice176::Candidate &c, in.candidates)
			body += candidate_to_line(c);
		out += line_wrap(body.join(";"), 78);
	}
	out += "-----END ICE-----";
	return out;
}

static IceOffer iceblock_parse(const QStringList &in)
{
	IceOffer out;
	if(in.count() < 3 || in[0] != "-----BEGIN ICE-----" || in[in.count()-1] != "-----END ICE-----")
		return IceOffer();

	QStringList body = lines_unwrap(in.mid(1, in.count() - 2)).split(';');
	if(body.count() < 2)
		return IceOffer();

	QStringList parts = body[0].split(',');
	if(parts.count() != 2)
		return IceOffer();
	bool ok;
	out.user = urlishDecode(parts[0], &ok);
	if(!ok || out.user.isEmpty())
		return IceOffer();
	out.pass = urlishDecode(parts[1], &ok);
	if(!ok || out.pass.isEmpty())
		return IceOffer();

	for(int n = 1; n < body.count(); ++n)
	{
		XMPP::Ice176::Candidate c = line_to_candidate(body[n]);
		if(c.type.isEmpty())
			return IceOffer();
		out.candidates += c;
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
		if(fgets(line.data(), line.size(), fp) == NULL)
			return QStringList();
		if(feof(fp))
			break;

		// hack off newline
		line.resize(qstrlen(line.data()) - 1);

		QString str = QString::fromLocal8Bit(line);
		out += str;
		if(str == "-----END ICE-----")
			break;
	}

	return out;
}

static void wait_for_enter()
{
	QByteArray buf(1024, 0);
	if(fgets(buf.data(), buf.size(), stdin) == NULL)
		return;
}

class App : public QObject
{
	Q_OBJECT

public:
	class Channel
	{
	public:
		QUdpSocket *sock;
		bool ready;
	};

	int opt_mode;
	int opt_localBase;
	int opt_channels;
	QString opt_stunHost;
	int opt_stunPort;
	bool opt_is_relay;
	QString opt_user, opt_pass;

	XMPP::NameResolver dns;
	QHostAddress stunAddr;
	XMPP::Ice176 *ice;
	QList<XMPP::Ice176::LocalAddress> localAddrs;
	QList<Channel> channels;

	App()
	{
	}

	~App()
	{
		delete ice;

		for(int n = 0; n < channels.count(); ++n)
		{
			channels[n].sock->disconnect(this);
			channels[n].sock->setParent(0);
			channels[n].sock->deleteLater();
			channels[n].sock = 0;
		}
	}

public slots:
	void start()
	{
		connect(&dns, SIGNAL(resultsReady(const QList<XMPP::NameRecord> &)), SLOT(dns_resultsReady(const QList<XMPP::NameRecord> &)));
		connect(&dns, SIGNAL(error(XMPP::NameResolver::Error)), SLOT(dns_error(XMPP::NameResolver::Error)));

		if(!opt_stunHost.isEmpty())
			dns.start(opt_stunHost.toLatin1(), XMPP::NameRecord::A);
		else
			start_ice();
	}

public:
	void start_ice()
	{
		ice = new XMPP::Ice176(this);
		connect(ice, SIGNAL(started()), SLOT(ice_started()));
		connect(ice, SIGNAL(localCandidatesReady(const QList<XMPP::Ice176::Candidate> &)), SLOT(ice_localCandidatesReady(const QList<XMPP::Ice176::Candidate> &)));
		connect(ice, SIGNAL(componentReady(int)), SLOT(ice_componentReady(int)));
		connect(ice, SIGNAL(readyRead(int)), SLOT(ice_readyRead(int)));
		connect(ice, SIGNAL(datagramsWritten(int, int)), SLOT(ice_datagramsWritten(int, int)));

		ice->setBasePort(opt_localBase + 64);

		QStringList strList;
		{
			/*XMPP::NetInterfaceManager netman;
			foreach(const QString &id, netman.interfaces())
			{
				XMPP::NetInterface ni(id, &netman);

				QHostAddress v4addr;
				foreach(const QHostAddress &h, ni.addresses())
				{
					if(h.protocol() == QAbstractSocket::IPv4Protocol)
					{
						v4addr = h;
						break;
					}
				}

				if(!v4addr.isNull())
				{
					XMPP::Ice176::LocalAddress addr;
					addr.addr = v4addr;
					localAddrs += addr;
					strList += addr.addr.toString();
				}
			}*/
			foreach(const QHostAddress &h, QNetworkInterface::allAddresses())
			{
				XMPP::Ice176::LocalAddress addr;
				addr.addr = h;
				localAddrs += addr;
				strList += addr.addr.toString();
			}
		}
		/*{
			XMPP::Ice176::LocalAddress addr;
			addr.addr = QHostAddress::LocalHost;
			localAddrs += addr;
			strList += addr.addr.toString();
		}*/
		ice->setLocalAddresses(localAddrs);

		printf("Interfaces: %s\n", qPrintable(strList.join(", ")));

		for(int n = 0; n < opt_channels; ++n)
		{
			Channel chan;
			chan.sock = new QUdpSocket(this);
			connect(chan.sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
			connect(chan.sock, SIGNAL(bytesWritten(qint64)), SLOT(sock_bytesWritten(qint64)));
			if(!chan.sock->bind(opt_localBase + 32 + n))
			{
				printf("Unable to bind to port %d.\n", opt_localBase + 32);
				emit quit();
				return;
			}
			chan.ready = false;

			channels += chan;
		}

		ice->setComponentCount(opt_channels);

		if(!stunAddr.isNull())
		{
			XMPP::Ice176::StunServiceType stunType;
			if(opt_is_relay)
				stunType = XMPP::Ice176::Relay;
			else
				stunType = XMPP::Ice176::Basic;
			ice->setStunService(stunType, stunAddr, opt_stunPort);
			if(!opt_user.isEmpty())
			{
				ice->setStunUsername(opt_user);
				ice->setStunPassword(opt_pass.toUtf8());
			}

			printf("STUN service: %s\n", qPrintable(stunAddr.toString()));
		}

		if(opt_mode == 0)
			ice->start(XMPP::Ice176::Initiator);
		else
			ice->start(XMPP::Ice176::Responder);
	}

signals:
	void quit();

private slots:
	void dns_resultsReady(const QList<XMPP::NameRecord> &results)
	{
		stunAddr = results.first().address();

		start_ice();
	}

	void dns_error(XMPP::NameResolver::Error e)
	{
		Q_UNUSED(e);
		printf("Unable to resolve stun host.\n");
		emit quit();
	}

	void ice_started()
	{
		if(channels.count() > 1)
		{
			printf("Local ports: %d-%d\n", opt_localBase, opt_localBase + channels.count() - 1);
			printf("Tunnel ports: %d-%d\n", opt_localBase + 32, opt_localBase + 32 + channels.count() - 1);
		}
		else
		{
			printf("Local port: %d\n", opt_localBase);
			printf("Tunnel port: %d\n", opt_localBase + 32);
		}
	}

	void ice_localCandidatesReady(const QList<XMPP::Ice176::Candidate> &list)
	{
		IceOffer out;
		out.user = ice->localUfrag();
		out.pass = ice->localPassword();
		out.candidates = list;
		QStringList block = iceblock_create(out);
		foreach(const QString &s, block)
			printf("%s\n", qPrintable(s));

		printf("Give above ICE block to peer.  Obtain peer ICE block and paste below...\n");
		IceOffer in = iceblock_parse(iceblock_read());
		if(in.user.isEmpty())
		{
			printf("Error parsing ICE block.\n");
			emit quit();
			return;
		}

		printf("Press enter to begin.\n");
		wait_for_enter();

		ice->setPeerUfrag(in.user);
		ice->setPeerPassword(in.pass);
		ice->addRemoteCandidates(in.candidates);
	}

	void ice_componentReady(int index)
	{
		printf("Channel %d ready.\n", index);
		channels[index].ready = true;

		bool allReady = true;
		for(int n = 0; n < channels.count(); ++n)
		{
			if(!channels[n].ready)
			{
				allReady = false;
				break;
			}
		}

		if(allReady)
		{
			printf("Tunnel established!\n");
		}
	}

	void ice_readyRead(int componentIndex)
	{
		while(ice->hasPendingDatagrams(componentIndex))
		{
			QByteArray buf = ice->readDatagram(componentIndex);
			channels[componentIndex].sock->writeDatagram(buf, QHostAddress::LocalHost, opt_localBase);
		}
	}

	void ice_datagramsWritten(int componentIndex, int count)
	{
		Q_UNUSED(componentIndex);
		Q_UNUSED(count);

		// do nothing
	}

	void sock_readyRead()
	{
		QUdpSocket *sock = (QUdpSocket *)sender();
		int at = -1;
		for(int n = 0; n < channels.count(); ++n)
		{
			if(channels[n].sock == sock)
			{
				at = n;
				break;
			}
		}
		Q_ASSERT(at != -1);

		while(sock->hasPendingDatagrams())
		{
			QByteArray buf;
			buf.resize(sock->pendingDatagramSize());

			// note: we don't care who sent it
			sock->readDatagram(buf.data(), buf.size());

			if(channels[at].ready)
				ice->writeDatagram(at, buf);
		}
	}

	void sock_bytesWritten(qint64 bytes)
	{
		Q_UNUSED(bytes);

		// do nothing
	}
};

void usage()
{
	printf("icetunnel: create a peer-to-peer UDP tunnel based on ICE\n");
	printf("usage: icetunnel initiator (options)\n");
	printf("       icetunnel responder (options)\n");
	printf("\n");
	printf(" --localbase=[n]     local base port (default=60000)\n");
	printf(" --channels=[n]      number of channels to create (default=4)\n");
	printf(" --stunhost=[host]   STUN server to use\n");
	printf(" --stunport=[n]      STUN server port to use (default=3478)\n");
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
	int stunPort = 3478;
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
		{
			channels = val.toInt();
			if(channels < 1 || channels > 32)
			{
				fprintf(stderr, "Number of channels must be between 1-32.\n");
				return 1;
			}
		}
		else if(var == "stunhost")
			stunHost = val;
		else if(var == "stunport")
			stunPort = val.toInt();
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

	if(!QCA::isSupported("hmac(sha1)"))
	{
		printf("Error: Need hmac(sha1) support.\n");
		return 1;
	}

	App app;
	app.opt_mode = mode;
	app.opt_localBase = localBase;
	app.opt_channels = channels;
	app.opt_stunHost = stunHost;
	app.opt_stunPort = stunPort;
	app.opt_is_relay = is_relay;
	app.opt_user = user;
	app.opt_pass = pass;

	QObject::connect(&app, SIGNAL(quit()), &qapp, SLOT(quit()));
	QTimer::singleShot(0, &app, SLOT(start()));
	qapp.exec();

	return 0;
}

#include "main.moc"
