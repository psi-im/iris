/*
 * bsocket.cpp - QSocket wrapper based on Bytestream with SRV DNS support
 * Copyright (C) 2003  Justin Karneges
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

#include <QTcpSocket>
#include <QHostAddress>
#include <QMetaType>

#include <limits>

#include "bsocket.h"

//#include "safedelete.h"

//#define BS_DEBUG

#ifdef BS_DEBUG
# define BSDEBUG (qDebug() << this << "#" << __FUNCTION__ << ":")
#endif


#define READBUFSIZE 65536

// CS_NAMESPACE_BEGIN

class QTcpSocketSignalRelay : public QObject
{
	Q_OBJECT
public:
	QTcpSocketSignalRelay(QTcpSocket *sock, QObject *parent = 0)
	:QObject(parent)
	{
		qRegisterMetaType<QAbstractSocket::SocketError>("QAbstractSocket::SocketError");
		connect(sock, SIGNAL(hostFound()), SLOT(sock_hostFound()), Qt::QueuedConnection);
		connect(sock, SIGNAL(connected()), SLOT(sock_connected()), Qt::QueuedConnection);
		connect(sock, SIGNAL(disconnected()), SLOT(sock_disconnected()), Qt::QueuedConnection);
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()), Qt::QueuedConnection);
		connect(sock, SIGNAL(bytesWritten(qint64)), SLOT(sock_bytesWritten(qint64)), Qt::QueuedConnection);
		connect(sock, SIGNAL(error(QAbstractSocket::SocketError)), SLOT(sock_error(QAbstractSocket::SocketError)), Qt::QueuedConnection);
	}

signals:
	void hostFound();
	void connected();
	void disconnected();
	void readyRead();
	void bytesWritten(qint64);
	void error(QAbstractSocket::SocketError);

public slots:
	void sock_hostFound()
	{
		emit hostFound();
	}

	void sock_connected()
	{
		emit connected();
	}

	void sock_disconnected()
	{
		emit disconnected();
	}

	void sock_readyRead()
	{
		emit readyRead();
	}

	void sock_bytesWritten(qint64 x)
	{
		emit bytesWritten(x);
	}

	void sock_error(QAbstractSocket::SocketError x)
	{
		emit error(x);
	}
};

static XMPP::NameRecord::Type chooseQuery(QAbstractSocket::NetworkLayerProtocol protocol, BSocket::Protocol defaultProtocol) {
	switch (protocol) {
		case QAbstractSocket::UnknownNetworkLayerProtocol:
			if (defaultProtocol == BSocket::IPv6_IPv4 || defaultProtocol == BSocket::IPv6)
				return XMPP::NameRecord::Aaaa;
			else
				return XMPP::NameRecord::A;
			break;
		case QAbstractSocket::IPv6Protocol:
			return XMPP::NameRecord::Aaaa;
			break;
		case QAbstractSocket::IPv4Protocol:
			return XMPP::NameRecord::A;
			break;
	}

	return XMPP::NameRecord::Any;
}

class BSocket::Private
{
public:
	Private()
	{
		qsock = 0;
		qsock_relay = 0;
		protocol = IPv6_IPv4;
	}

	QTcpSocket *qsock;
	QTcpSocketSignalRelay *qsock_relay;
	int state;
	Protocol protocol; //!< IP protocol to use

	QString domain; //!< Domain we are currently connected to
	QString host; //!< Hostname we are currently connected to
	QHostAddress address; //!< IP address we are currently connected to
	quint16 port; //!< Port we are currently connected to

	struct {
		QString host;
		quint16 port;
	} failsafeHost;

	//SafeDelete sd;

	XMPP::WeightedNameRecordList srvList; //!< List of resolved SRV names
	QList<XMPP::NameRecord> hostList; //!< List or resolved hostnames for current SRV name
	QList<XMPP::NameResolver*> resolverList; //!< NameResolvers currently in use, needed for cleanup
};

BSocket::BSocket(QObject *parent)
:ByteStream(parent)
{
	d = new Private;
	reset();
}

BSocket::~BSocket()
{
	reset(true);
	delete d;
}

void BSocket::reset(bool clear)
{
	if(d->qsock) {
		delete d->qsock_relay;
		d->qsock_relay = 0;

		/*d->qsock->disconnect(this);

		if(!clear && d->qsock->isOpen() && d->qsock->isValid()) {*/
			// move remaining into the local queue
			QByteArray block(d->qsock->bytesAvailable(), 0);
			d->qsock->read(block.data(), block.size());
			appendRead(block);
		//}

		//d->sd.deleteLater(d->qsock);
    d->qsock->deleteLater();
		d->qsock = 0;
	}
	else {
		if(clear)
			clearReadBuffer();
	}

	/* cleanup resolvers */
	foreach (XMPP::NameResolver *resolver, d->resolverList) {
		cleanup_resolver(resolver);
	}

	d->state = Idle;
}

void BSocket::ensureSocket()
{
	if(!d->qsock) {
		d->qsock = new QTcpSocket;
#if QT_VERSION >= 0x030200
		d->qsock->setReadBufferSize(READBUFSIZE);
#endif
		d->qsock_relay = new QTcpSocketSignalRelay(d->qsock);
		connect(d->qsock_relay, SIGNAL(hostFound()), SLOT(qs_hostFound()));
		connect(d->qsock_relay, SIGNAL(connected()), SLOT(qs_connected()));
		connect(d->qsock_relay, SIGNAL(disconnected()), SLOT(qs_closed()));
		connect(d->qsock_relay, SIGNAL(readyRead()), SLOT(qs_readyRead()));
		connect(d->qsock_relay, SIGNAL(bytesWritten(qint64)), SLOT(qs_bytesWritten(qint64)));
		connect(d->qsock_relay, SIGNAL(error(QAbstractSocket::SocketError)), SLOT(qs_error(QAbstractSocket::SocketError)));
	}
}

void BSocket::cleanup_resolver(XMPP::NameResolver *resolver) {
	disconnect(resolver);
	resolver->stop();
	resolver->deleteLater();
	d->resolverList.removeAll(resolver);
}

bool BSocket::check_protocol_fallback() {
	return (d->protocol == IPv6_IPv4 && d->address.protocol() == QAbstractSocket::IPv6Protocol)
		|| (d->protocol == IPv4_IPv6 && d->address.protocol() == QAbstractSocket::IPv4Protocol);
}

void BSocket::connectToHost(const QHostAddress &address, quint16 port)
{
#ifdef BS_DEBUG
	BSDEBUG << "a:" << address << "p:" << port;
#endif

	reset(true);
	d->address = address;
	d->port = port;
	d->state = Connecting;

	ensureSocket();
	d->qsock->connectToHost(address, port);
}

void BSocket::connectToHost(const QString &host, quint16 port, QAbstractSocket::NetworkLayerProtocol protocol)
{
#ifdef BS_DEBUG
	BSDEBUG << "h:" << host << "p:" << port << "pr:" << protocol;
#endif

	reset(true);
	d->host = host;
	d->port = port;
	d->state = HostLookup;

	XMPP::NameRecord::Type querytype = chooseQuery(protocol, d->protocol);
	XMPP::NameResolver *resolver = new XMPP::NameResolver;
	connect(resolver, SIGNAL(resultsReady(QList<XMPP::NameRecord>)), this, SLOT(handle_dns_host_ready(QList<XMPP::NameRecord>)));
	connect(resolver, SIGNAL(error(XMPP::NameResolver::Error)), this, SLOT(handle_dns_host_error(XMPP::NameResolver::Error)));
	resolver->start(host.toLocal8Bit(), querytype);
	d->resolverList << resolver;
}

void BSocket::connectToHost(const QString &service, const QString &transport, const QString &domain)
{
#ifdef BS_DEBUG
	BSDEBUG << "s:" << service << "t:" << transport << "d:" << domain;
#endif

	QString srv_request("_" + service + "._" + transport + "." + domain + ".");

	reset(true);
	d->domain = domain;
	d->state = HostLookup;

	XMPP::NameResolver *resolver = new XMPP::NameResolver;
	connect(resolver, SIGNAL(resultsReady(QList<XMPP::NameRecord>)), this, SLOT(handle_dns_srv_ready(QList<XMPP::NameRecord>)));
	connect(resolver, SIGNAL(error(XMPP::NameResolver::Error)), this, SLOT(handle_dns_srv_error(XMPP::NameResolver::Error)));
	resolver->start(srv_request.toLocal8Bit(), XMPP::NameRecord::Srv);
	d->resolverList << resolver;
}

/* SRV request resolved, now try to connect to the hosts */
void BSocket::handle_dns_srv_ready(const QList<XMPP::NameRecord> &r)
{
#ifdef BS_DEBUG
	BSDEBUG << "sl:" << r;
#endif

	QList<XMPP::NameRecord> rl(r);

	/* cleanup resolver */
	cleanup_resolver(static_cast<XMPP::NameResolver*>(sender()));

	/* After we tried all SRV hosts, try connecting directly */
	XMPP::NameRecord failsafeHost(d->domain.toLocal8Bit(), std::numeric_limits<int>::max());
	failsafeHost.setSrv(d->failsafeHost.host.toLocal8Bit(), d->failsafeHost.port, std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
#ifdef BS_DEBUG
	BSDEBUG << "Adding failsafe:" << failsafeHost;
#endif
	rl << failsafeHost;

	/* lookup srv pointers */
	d->srvList = rl;
	dns_srv_try_next();
}

/* failed the srv lookup, fall back to simple lookup */
void BSocket::handle_dns_srv_error(XMPP::NameResolver::Error e)
{
#ifdef BS_DEBUG
	BSDEBUG << "e:" << e;
#endif

	/* cleanup resolver */
	cleanup_resolver(static_cast<XMPP::NameResolver*>(sender()));

	/* try connecting directly */
	connectToHost(d->failsafeHost.host, d->failsafeHost.port);
}

/* hosts resolved, now try to connect to them */
void BSocket::handle_dns_host_ready(const QList<XMPP::NameRecord> &r)
{
#ifdef BS_DEBUG
	BSDEBUG << "hl:" << r;
#endif

	/* cleanup resolver */
	cleanup_resolver(static_cast<XMPP::NameResolver*>(sender()));

	/* connect to host */
	d->hostList = r;
	connect_host_try_next();
}

/* failed to lookup the primary record (A or AAAA, depending on user choice) */
void BSocket::handle_dns_host_error(XMPP::NameResolver::Error e)
{
#ifdef BS_DEBUG
	BSDEBUG << "e:" << e;
#endif

	/* cleanup resolver */
	cleanup_resolver(static_cast<XMPP::NameResolver*>(sender()));

	/* if a fallback is requested, try that */
	if (d->protocol == IPv6_IPv4 || d->protocol == IPv4_IPv6) {
		XMPP::NameRecord::Type querytype = (d->protocol == IPv6_IPv4 ? XMPP::NameRecord::A : XMPP::NameRecord::Aaaa);

		XMPP::NameResolver *resolver = new XMPP::NameResolver;
		connect(resolver, SIGNAL(resultsReady(QList<XMPP::NameRecord>)), this, SLOT(handle_dns_host_ready(QList<XMPP::NameRecord>)));
		connect(resolver, SIGNAL(error(XMPP::NameResolver::Error)), this, SLOT(handle_dns_host_fallback_error(XMPP::NameResolver::Error)));
		resolver->start(d->host.toLocal8Bit(), querytype);
		d->resolverList << resolver;
	}
	/* there is no fallback requested */
	else {
		/* no-fallback should behave the same as a failed fallback */
		handle_dns_host_fallback_error(e);
	}
}

/* failed to lookup the fallback record (A or AAAA, depending on user choice) */
void BSocket::handle_dns_host_fallback_error(XMPP::NameResolver::Error e)
{
#ifdef BS_DEBUG
	BSDEBUG << "e:" << e;
#endif

	/* cleanup resolver */
	cleanup_resolver(static_cast<XMPP::NameResolver*>(sender()));

	/* lookup next host via SRV */
	dns_srv_try_next();
}

/* failed to connect to host */
void BSocket::handle_connect_error(QAbstractSocket::SocketError e) {
#ifdef BS_DEBUG
	BSDEBUG << "e:" << e;
#endif

	if (check_protocol_fallback()) {
		QAbstractSocket::NetworkLayerProtocol protocol =
			(d->protocol == BSocket::IPv6_IPv4 ? QAbstractSocket::IPv4Protocol : QAbstractSocket::IPv6Protocol);
		connectToHost(d->host, d->port, protocol);
	}
	/* connect to next host */
	else if (!connect_host_try_next()) {
		/* the DNS names are already resolved, we have some kind of network error here */
		/* SRV department shall decide whether we quit here... */
		dns_srv_try_next();
	}
}

/* lookup the next SRV record in line */
void BSocket::dns_srv_try_next()
{
#ifdef BS_DEBUG
	BSDEBUG << "sl:" << d->srvList;
#endif

	if (!d->srvList.empty()) {
		XMPP::NameRecord record(d->srvList.takeNext());
		/* lookup host by name and specify port for later use */
		connectToHost(record.name(), record.port());
	}
	else {
#ifdef BS_DEBUG
		BSDEBUG << "SRV list empty, failing";
#endif
		/* no more SRV hosts to try, fail */
		emit error(ErrConnectionRefused);
	}
}

/*!
	connect to the next resolved host in line
	\return true if found another host in list, false otherwise
*/
bool BSocket::connect_host_try_next() {
#ifdef BS_DEBUG
	BSDEBUG << "hl:" << d->hostList;
#endif

	if (!d->hostList.empty()) {
		XMPP::NameRecord record(d->hostList.takeFirst());
		/* connect to address directly on the port specified earlier */
		connectToHost(record.address(), d->port);
		return true;
	}
	else {
#ifdef BS_DEBUG
		BSDEBUG << "Host list empty, failing";
#endif
		return false;
	}
}

int BSocket::socket() const
{
	if(d->qsock)
		return d->qsock->socketDescriptor();
	else
		return -1;
}

void BSocket::setSocket(int s)
{
	reset(true);
	ensureSocket();
	d->state = Connected;
	d->qsock->setSocketDescriptor(s);
}

int BSocket::state() const
{
	return d->state;
}

BSocket::Protocol BSocket::protocol() const {
	return d->protocol;
}

void BSocket::setProtocol(BSocket::Protocol p) {
	d->protocol = p;
}

void BSocket::setFailsafeHost(const QString &host, quint16 port) {
#ifdef BS_DEBUG
	BSDEBUG << "h:" << host << "p:" << port;
#endif
	d->failsafeHost.host = host;
	d->failsafeHost.port = port;
}

bool BSocket::isOpen() const
{
	if(d->state == Connected)
		return true;
	else
		return false;
}

void BSocket::close()
{
	if(d->state == Idle)
		return;

	if(d->qsock) {
		d->qsock->close();
		d->state = Closing;
		if(d->qsock->bytesToWrite() == 0)
			reset();
	}
	else {
		reset();
	}
}

void BSocket::write(const QByteArray &a)
{
	if(d->state != Connected)
		return;
#ifdef BS_DEBUG_EXTRA
	BSDEBUG << "- [" << a.size() << "]: {" << a << "}";
#endif
	d->qsock->write(a.data(), a.size());
}

QByteArray BSocket::read(int bytes)
{
	QByteArray block;
	if(d->qsock) {
		int max = bytesAvailable();
		if(bytes <= 0 || bytes > max)
			bytes = max;
		block.resize(bytes);
		d->qsock->read(block.data(), block.size());
	}
	else
		block = ByteStream::read(bytes);

#ifdef BS_DEBUG_EXTRA
	BSDEBUG << "- [" << block.size() << "]: {" << block << "}";
#endif
	return block;
}

int BSocket::bytesAvailable() const
{
	if(d->qsock)
		return d->qsock->bytesAvailable();
	else
		return ByteStream::bytesAvailable();
}

int BSocket::bytesToWrite() const
{
	if(!d->qsock)
		return 0;
	return d->qsock->bytesToWrite();
}

QHostAddress BSocket::address() const
{
	if(d->qsock)
		return d->qsock->localAddress();
	else
		return QHostAddress();
}

quint16 BSocket::port() const
{
	if(d->qsock)
		return d->qsock->localPort();
	else
		return 0;
}

QHostAddress BSocket::peerAddress() const
{
	if(d->qsock)
		return d->qsock->peerAddress();
	else
		return QHostAddress();
}

quint16 BSocket::peerPort() const
{
	if(d->qsock)
		return d->qsock->peerPort();
	else
		return 0;
}

void BSocket::qs_hostFound()
{
	//SafeDeleteLock s(&d->sd);
}

void BSocket::qs_connected()
{
	d->state = Connected;
#ifdef BS_DEBUG
	BSDEBUG << "Connected";
#endif
	//SafeDeleteLock s(&d->sd);
	emit connected();
}

void BSocket::qs_closed()
{
	if(d->state == Closing)
	{
#ifdef BS_DEBUG
		BSDEBUG << "Delayed Close Finished";
#endif
		//SafeDeleteLock s(&d->sd);
		reset();
		emit delayedCloseFinished();
	}
}

void BSocket::qs_readyRead()
{
	//SafeDeleteLock s(&d->sd);
	emit readyRead();
}

void BSocket::qs_bytesWritten(qint64 x64)
{
	int x = x64;
#ifdef BS_DEBUG_EXTRA
	BSDEBUG << "BytesWritten [" << x << "]";
#endif
	//SafeDeleteLock s(&d->sd);
	emit bytesWritten(x);
}

void BSocket::qs_error(QAbstractSocket::SocketError x)
{
	/* arriving here from connectToHost() */
	if (d->state == Connecting) {
		/* We do our own special error handling in this case */
		handle_connect_error(x);
		return;
	}

	if(x == QTcpSocket::RemoteHostClosedError) {
#ifdef BS_DEBUG
		BSDEBUG << "Connection Closed";
#endif
		//SafeDeleteLock s(&d->sd);
		reset();
		emit connectionClosed();
		return;
	}

#ifdef BS_DEBUG
	BSDEBUG << "Error";
#endif
	//SafeDeleteLock s(&d->sd);

	reset();
	if(x == QTcpSocket::ConnectionRefusedError)
		emit error(ErrConnectionRefused);
	else if(x == QTcpSocket::HostNotFoundError)
		emit error(ErrHostNotFound);
	else
		emit error(ErrRead);
}

#include "bsocket.moc"

// CS_NAMESPACE_END
