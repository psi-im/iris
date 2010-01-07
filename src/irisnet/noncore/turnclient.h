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

class StunAllocate;

class TurnClient : public QObject
{
	Q_OBJECT

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
		ErrorAuth,
		ErrorRejected,
		ErrorProtocol,
		ErrorCapacity,

		// according to the TURN spec, a client should try three times
		//   to correct a mismatch error before giving up.  this class
		//   will perform the retries internally, and ErrorMismatch is
		//   only emitted when it has given up.  note that if this
		//   happens, the TURN spec says you should not connect to the
		//   TURN server again for at least 2 minutes.
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

	StunAllocate *stunAllocate();

	void addChannelPeer(const QHostAddress &addr, int port);

	int packetsToRead() const;
	int packetsToWrite() const;

	QByteArray read(QHostAddress *addr, int *port);
	void write(const QByteArray &buf, const QHostAddress &addr, int port);

	QString errorString() const;

signals:
	void connected(); // tcp connected
	void tlsHandshaken();
	void closed();
	void needAuthParams();
	void retrying(); // mismatch error received, starting all over
	void activated(); // ready for read/write
	void readyRead();
	void packetsWritten(int count, const QHostAddress &addr, int port);
	void error(XMPP::TurnClient::Error e);

	// not DOR-SS
	void debugLine(const QString &line);

private:
	class Private;
	friend class Private;
	Private *d;
};

}

#endif
