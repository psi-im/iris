/*
 * Copyright (C) 2010  Barracuda Networks, Inc.
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

#ifndef TURNCLIENT_H
#define TURNCLIENT_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QHostAddress>

namespace QCA {
	class SecureArray;
}

namespace XMPP {

class TurnClient : public QObject
{
public:
	enum Error
	{
		ErrorGeneric,
		ErrorHostNotFound,
		ErrorConnect,

		// stream error or stream unexpectedly disconnected by peer
		ErrorStream,

		ErrorProxyConnect,
		ErrorProxyNeg,
		ErrorProxyAuth,
		ErrorTls,
		ErrorRejected,
		ErrorProtocol,
		ErrorCapacity,

		// only happens if we can't resolve the problem internally
		ErrorMismatch
	};

	enum Mode
	{
		PlainMode,
		TlsMode
	};

	// adapted from XMPP::AdvancedConnector
	class Proxy
	{
	public:
		enum
		{
			None,
			HttpConnect,
			Socks
		};

		Proxy();
		~Proxy();

		int type() const;
		QString host() const;
		quint16 port() const;
		QString user() const;
		QString pass() const;

		void setHttpConnect(const QString &host, quint16 port);
		void setSocks(const QString &host, quint16 port);
		void setUserPass(const QString &user, const QString &pass);

	private:
		int t;
		QString v_host;
		quint16 v_port;
		QString v_user, v_pass;
	};

	TurnClient(QObject *parent = 0);
	~TurnClient();

	void setProxy(const Proxy &proxy);
	void setClientSoftwareNameAndVersion(const QString &str);

	void connectToHost(const QString &host, int port, Mode mode = PlainMode);

	QString realm() const;
	void setUsername(const QString &username);
	void setPassword(const QCA::SecureArray &password);
	void setRealm(const QString &realm);
	void continueAfterParams();

	void close();

	int packetsToRead() const;
	int packetsToWrite() const;

	QByteArray read(QHostAddress *addr, int *port);
	void write(const QByteArray &buf, const QHostAddress &addr, int port);

signals:
	void connected();
	void tlsHandshaken();
	void closed();
	void needAuthParams();
	void activated();
	void readyRead();
	void packetsWritten(int count);
	void error(XMPP::TurnClient::Error e);

private:
	class Private;
	friend class Private;
	Private *d;
};

}

#endif
