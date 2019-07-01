/*
 * socks.cpp - SOCKS5 TCP proxy client/server
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "socks.h"

#include <QHostAddress>
#include <QStringList>
#include <QTimer>
#include <QPointer>
#include <QSocketNotifier>
#include <QByteArray>

#ifdef Q_OS_UNIX
#include <sys/types.h>
#include <netinet/in.h>
#endif

#ifdef Q_OS_WIN32
#include <windows.h>
#endif

#ifdef Q_OS_UNIX
#include <unistd.h>
#include <fcntl.h>
#endif

#include "bsocket.h"

//#define PROX_DEBUG

#ifdef PROX_DEBUG
#include <stdio.h>
#endif

// CS_NAMESPACE_BEGIN

//----------------------------------------------------------------------------
// SocksUDP
//----------------------------------------------------------------------------

class SocksUDP::Private
{
public:
    QUdpSocket *sd;
    SocksClient *sc;
    QHostAddress routeAddr;
    int routePort;
    QString host;
    int port;
};

SocksUDP::SocksUDP(SocksClient *sc, const QString &host, int port, const QHostAddress &routeAddr, int routePort)
:QObject(sc)
{
    d = new Private;
    d->sc = sc;
    d->sd = new QUdpSocket(this);
    connect(d->sd, SIGNAL(readyRead()), SLOT(sd_readyRead()));
    d->host = host;
    d->port = port;
    d->routeAddr = routeAddr;
    d->routePort = routePort;
}

SocksUDP::~SocksUDP()
{
    delete d->sd;
    delete d;
}

void SocksUDP::change(const QString &host, int port)
{
    d->host = host;
    d->port = port;
}

void SocksUDP::write(const QByteArray &data)
{
    d->sd->writeDatagram(data, d->routeAddr, d->routePort);
}

void SocksUDP::sd_activated()
{
    while (d->sd->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(d->sd->pendingDatagramSize());
        d->sd->readDatagram(datagram.data(), datagram.size());
        packetReady(datagram);
    }
}

//----------------------------------------------------------------------------
// SocksClient
//----------------------------------------------------------------------------
#define REQ_CONNECT      0x01
#define REQ_BIND         0x02
#define REQ_UDPASSOCIATE 0x03

#define RET_SUCCESS      0x00
#define RET_UNREACHABLE  0x04
#define RET_CONNREFUSED  0x05

// spc = socks packet client
// sps = socks packet server
// SPCS = socks packet client struct
// SPSS = socks packet server struct

// Version
static QByteArray spc_set_version(bool hasCreds)
{
    QByteArray ver;
    ver.resize(hasCreds? 4 : 3);
    ver[0] = 0x05; // socks version 5
    ver[2] = 0x00; // no-auth
    if (hasCreds) {
        ver[1] = 0x02; // number of methods
        ver[3] = 0x02; // username
    } else {
        ver[1] = 0x01; // number of methods
    }
    return ver;
}

static QByteArray sps_set_version(int method)
{
    QByteArray ver;
    ver.resize(2);
    ver[0] = 0x05;
    ver[1] = method;
    return ver;
}

struct SPCS_VERSION
{
    unsigned char version;
    QByteArray methodList;
};

static int spc_get_version(QByteArray &from, SPCS_VERSION *s)
{
    if(from.size() < 1)
        return 0;
    if(from.at(0) != 0x05) // only SOCKS5 supported
        return -1;
    if(from.size() < 2)
        return 0;
    unsigned char mlen = from.at(1);
    int num = mlen;
    if(num > 16) // who the heck has over 16 auth methods??
        return -1;
    if(from.size() < 2 + num)
        return 0;
    QByteArray a = ByteStream::takeArray(from, 2+num);
    s->version = a[0];
    s->methodList.resize(num);
    memcpy(s->methodList.data(), a.data() + 2, num);
    return 1;
}

struct SPSS_VERSION
{
    unsigned char version;
    unsigned char method;
};

static int sps_get_version(QByteArray &from, SPSS_VERSION *s)
{
    if(from.size() < 2)
        return 0;
    QByteArray a = ByteStream::takeArray(from, 2);
    s->version = a[0];
    s->method = a[1];
    return 1;
}

// authUsername
static QByteArray spc_set_authUsername(const QByteArray &user, const QByteArray &pass)
{
    int len1 = user.length();
    int len2 = pass.length();
    if(len1 > 255)
        len1 = 255;
    if(len2 > 255)
        len2 = 255;
    QByteArray a;
    a.resize(1+1+len1+1+len2);
    a[0] = 0x01; // username auth version 1
    a[1] = len1;
    memcpy(a.data() + 2, user.data(), len1);
    a[2+len1] = len2;
    memcpy(a.data() + 3 + len1, pass.data(), len2);
    return a;
}

static QByteArray sps_set_authUsername(bool success)
{
    QByteArray a;
    a.resize(2);
    a[0] = 0x01;
    a[1] = success ? 0x00 : 0xff;
    return a;
}

struct SPCS_AUTHUSERNAME
{
    QString user, pass;
};

static int spc_get_authUsername(QByteArray &from, SPCS_AUTHUSERNAME *s)
{
    if(from.size() < 1)
        return 0;
    unsigned char ver = from.at(0);
    if(ver != 0x01)
        return -1;
    if(from.size() < 2)
        return 0;
    unsigned char ulen = from.at(1);
    if((int)from.size() < ulen + 3)
        return 0;
    unsigned char plen = from.at(ulen+2);
    if((int)from.size() < ulen + plen + 3)
        return 0;
    QByteArray a = ByteStream::takeArray(from, ulen + plen + 3);

    QByteArray user, pass;
    user.resize(ulen);
    pass.resize(plen);
    memcpy(user.data(), a.data()+2, ulen);
    memcpy(pass.data(), a.data()+ulen+3, plen);
    s->user = QString::fromUtf8(user);
    s->pass = QString::fromUtf8(pass);
    return 1;
}

struct SPSS_AUTHUSERNAME
{
    unsigned char version;
    bool success;
};

static int sps_get_authUsername(QByteArray &from, SPSS_AUTHUSERNAME *s)
{
    if(from.size() < 2)
        return 0;
    QByteArray a = ByteStream::takeArray(from, 2);
    s->version = a[0];
    s->success = ((char) a[1] == 0 ? true: false);
    return 1;
}

// connectRequest
static QByteArray sp_set_request(const QHostAddress &addr, unsigned short port, unsigned char cmd1)
{
    int at = 0;
    QByteArray a;
    a.resize(4);
    a[at++] = 0x05; // socks version 5
    a[at++] = cmd1;
    a[at++] = 0x00; // reserved
    if(addr.protocol() == QAbstractSocket::IPv4Protocol || addr.protocol() == QAbstractSocket::UnknownNetworkLayerProtocol) {
        a[at++] = 0x01; // address type = ipv4
        quint32 ip4 = htonl(addr.toIPv4Address());
        a.resize(at+4);
        memcpy(a.data() + at, &ip4, 4);
        at += 4;
    }
    else {
        a[at++] = 0x04;
        Q_IPV6ADDR ip6 = addr.toIPv6Address();
        a.resize(at+16);
        for(int i = 0; i < 16; ++i)
            a[at++] = ip6[i];
    }

    // port
    a.resize(at+2);
    quint16 p = htons(port);
    memcpy(a.data() + at, &p, 2);

    return a;
}

static QByteArray sp_set_request(const QString &host, quint16 port, unsigned char cmd1)
{
    // detect for IP addresses
    QHostAddress addr;
    if(addr.setAddress(host))
        return sp_set_request(addr, port, cmd1);

    QByteArray h = host.toUtf8();
    h.truncate(255);
    h = QString::fromUtf8(h).toUtf8(); // delete any partial characters?
    int hlen = h.length();

    int at = 0;
    QByteArray a;
    a.resize(4);
    a[at++] = 0x05; // socks version 5
    a[at++] = cmd1;
    a[at++] = 0x00; // reserved
    a[at++] = 0x03; // address type = domain

    // host
    a.resize(at+hlen+1);
    a[at++] = hlen;
    memcpy(a.data() + at, h.data(), hlen);
    at += hlen;

    // port
    a.resize(at+2);
    unsigned short p = htons(port);
    memcpy(a.data() + at, &p, 2);

    return a;
}

struct SPS_CONNREQ
{
    unsigned char version;
    unsigned char cmd;
    int address_type;
    QString host;
    QHostAddress addr;
    quint16 port;
};

static int sp_get_request(QByteArray &from, SPS_CONNREQ *s)
{
    int full_len = 4;
    if((int)from.size() < full_len)
        return 0;

    QString host;
    QHostAddress addr;
    unsigned char atype = from.at(3);

    if(atype == 0x01) {
        full_len += 4;
        if((int)from.size() < full_len)
            return 0;
        quint32 ip4;
        memcpy(&ip4, from.data() + 4, 4);
        addr.setAddress(ntohl(ip4));
    }
    else if(atype == 0x03) {
        ++full_len;
        if((int)from.size() < full_len)
            return 0;
        unsigned char host_len = from.at(4);
        full_len += host_len;
        if((int)from.size() < full_len)
            return 0;
        QByteArray cs;
        cs.resize(host_len);
        memcpy(cs.data(), from.data() + 5, host_len);
        host = QString::fromLatin1(cs);
    }
    else if(atype == 0x04) {
        full_len += 16;
        if((int)from.size() < full_len)
            return 0;
        quint8 a6[16];
        memcpy(a6, from.data() + 4, 16);
        addr.setAddress(a6);
    }

    full_len += 2;
    if((int)from.size() < full_len)
        return 0;

    QByteArray a = ByteStream::takeArray(from, full_len);

    quint16 p;
    memcpy(&p, a.data() + full_len - 2, 2);

    s->version = a[0];
    s->cmd = a[1];
    s->address_type = atype;
    s->host = host;
    s->addr = addr;
    s->port = ntohs(p);

    return 1;
}

enum { StepVersion, StepAuth, StepRequest };

class SocksClient::Private
{
public:
    Private(SocksClient *_q) :
        sock(_q)
    {
    }

    BSocket sock;
    QString host;
    int port;
    QString user, pass;
    QString real_host;
    int real_port;

    QByteArray recvBuf;
    int step;
    int authMethod;
    bool incoming, waiting;

    QString rhost;
    int rport;

    int pending;

    bool udp;
    QString udpAddr;
    int udpPort;
};

SocksClient::SocksClient(QObject *parent)
:ByteStream(parent)
{
    init();

    d->incoming = false;
}

SocksClient::SocksClient(QTcpSocket *s, QObject *parent)
:ByteStream(parent)
{
    init();

    d->incoming = true;
    d->waiting = true;
    d->sock.setSocket(s);
}

void SocksClient::init()
{
    d = new Private(this);
    connect(&d->sock, SIGNAL(connected()), SLOT(sock_connected()));
    connect(&d->sock, SIGNAL(connectionClosed()), SLOT(sock_connectionClosed()));
    connect(&d->sock, SIGNAL(delayedCloseFinished()), SLOT(sock_delayedCloseFinished()));
    connect(&d->sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
    connect(&d->sock, SIGNAL(bytesWritten(qint64)), SLOT(sock_bytesWritten(qint64)));
    connect(&d->sock, SIGNAL(error(int)), SLOT(sock_error(int)));

    resetConnection(true);
}

SocksClient::~SocksClient()
{
    resetConnection(true);
    delete d;
}

QAbstractSocket* SocksClient::abstractSocket() const
{
    return d->sock.abstractSocket();
}

void SocksClient::resetConnection(bool clear)
{
    if(d->sock.state() != BSocket::Idle)
        d->sock.close();
    if(clear)
        clearReadBuffer();
    d->recvBuf.resize(0);
    d->waiting = false;
    d->udp = false;
    d->pending = 0;
    if (bytesAvailable()) {
        setOpenMode(QIODevice::ReadOnly);
    } else {
        setOpenMode(QIODevice::NotOpen);
    }
}

bool SocksClient::isIncoming() const
{
    return d->incoming;
}

void SocksClient::setAuth(const QString &user, const QString &pass)
{
    d->user = user;
    d->pass = pass;
}

void SocksClient::connectToHost(const QString &proxyHost, int proxyPort, const QString &host, int port, bool udpMode)
{
    resetConnection(true);

    d->host = proxyHost;
    d->port = proxyPort;
    d->real_host = host;
    d->real_port = port;
    d->udp = udpMode;

#ifdef PROX_DEBUG
    fprintf(stderr, "SocksClient: Connecting to %s:%d", qPrintable(proxyHost), proxyPort);
    if(d->user.isEmpty())
        fprintf(stderr, "\n");
    else
        fprintf(stderr, ", auth {%s,%s}\n", qPrintable(d->user), qPrintable(d->pass));
#endif
    d->sock.connectToHost(d->host, d->port);
}

void SocksClient::close()
{
    d->sock.close();
    if(d->sock.bytesToWrite() == 0)
        resetConnection();
}

void SocksClient::writeData(const QByteArray &buf)
{
#ifdef PROX_DEBUG
    // show hex
    fprintf(stderr, "SocksClient: client write { ");
    for(int n = 0; n < (int)buf.size(); ++n)
        fprintf(stderr, "%02X ", (unsigned char)buf[n]);
    fprintf(stderr, " } \n");
#endif
    d->pending += buf.size();
    d->sock.write(buf);
}

qint64 SocksClient::writeData(const char *data, qint64 maxSize)
{
    if(isOpen() && !d->udp)
        return d->sock.write(data, maxSize);
    return 0;
}

qint64 SocksClient::readData(char *data, qint64 maxSize)
{
    qint64 ret = ByteStream::readData(data, maxSize);
    if (d->sock.state() != BSocket::Connected && !bytesAvailable()) {
        setOpenMode(QIODevice::NotOpen);
    }
    return ret;
}

qint64 SocksClient::bytesAvailable() const
{
    return ByteStream::bytesAvailable();
}

qint64 SocksClient::bytesToWrite() const
{
    if(isOpen())
        return d->sock.bytesToWrite();
    else
        return 0;
}

void SocksClient::sock_connected()
{
#ifdef PROX_DEBUG
    fprintf(stderr, "SocksClient: Connected\n");
#endif

    d->step = StepVersion;
    writeData(spc_set_version(!d->user.isEmpty())); // fixme requirement for auth should set outside
}

void SocksClient::sock_connectionClosed()
{
    if(isOpen()) {
        resetConnection();
        emit connectionClosed();
    }
    else {
        setError(ErrProxyNeg);
    }
}

void SocksClient::sock_delayedCloseFinished()
{
    if(isOpen()) {
        resetConnection();
        delayedCloseFinished();
    }
}

void SocksClient::sock_readyRead()
{
    QByteArray block = d->sock.readAll();

    //qDebug() << this << "::sock_readyRead " << block.size() << " bytes." <<
    //            "udp=" << d->udp << openMode();
    if(!isOpen()) {
        if(d->incoming)
            processIncoming(block);
        else
            processOutgoing(block);
    }
    else {
        if(!d->udp) {
            appendRead(block);
            emit readyRead();
        }
    }
}

void SocksClient::processOutgoing(const QByteArray &block)
{
#ifdef PROX_DEBUG
    // show hex
    fprintf(stderr, "SocksClient: client recv { ");
    for(int n = 0; n < (int)block.size(); ++n)
        fprintf(stderr, "%02X ", (unsigned char)block[n]);
    fprintf(stderr, " } \n");
#endif
    d->recvBuf += block;

    if(d->step == StepVersion) {
        SPSS_VERSION s;
        int r = sps_get_version(d->recvBuf, &s);
        if(r == -1) {
            resetConnection(true);
            setError(ErrProxyNeg);
            return;
        }
        else if(r == 1) {
            if(s.version != 0x05 || s.method == 0xff) {
#ifdef PROX_DEBUG
                fprintf(stderr, "SocksClient: Method selection failed\n");
#endif
                resetConnection(true);
                setError(ErrProxyNeg);
                return;
            }

            QString str;
            if(s.method == 0x00) {
                str = "None";
                d->authMethod = AuthNone;
            }
            else if(s.method == 0x02) {
                str = "Username/Password";
                d->authMethod = AuthUsername;
            }
            else {
#ifdef PROX_DEBUG
                fprintf(stderr, "SocksClient: Server wants to use unknown method '%02x'\n", s.method);
#endif
                resetConnection(true);
                setError(ErrProxyNeg);
                return;
            }

            if(d->authMethod == AuthNone) {
                // no auth, go straight to the request
                do_request();
            }
            else if(d->authMethod == AuthUsername) {
                d->step = StepAuth;
#ifdef PROX_DEBUG
                fprintf(stderr, "SocksClient: Authenticating [Username] ...\n");
#endif
                writeData(spc_set_authUsername(d->user.toLatin1(), d->pass.toLatin1()));
            }
        }
    }
    if(d->step == StepAuth) {
        if(d->authMethod == AuthUsername) {
            SPSS_AUTHUSERNAME s;
            int r = sps_get_authUsername(d->recvBuf, &s);
            if(r == -1) {
                resetConnection(true);
                setError(ErrProxyNeg);
                return;
            }
            else if(r == 1) {
                if(s.version != 0x01) {
                    resetConnection(true);
                    setError(ErrProxyNeg);
                    return;
                }
                if(!s.success) {
                    resetConnection(true);
                    setError(ErrProxyAuth);
                    return;
                }

                do_request();
            }
        }
    }
    else if(d->step == StepRequest) {
        SPS_CONNREQ s;
        int r = sp_get_request(d->recvBuf, &s);
        if(r == -1) {
            resetConnection(true);
            setError(ErrProxyNeg);
            return;
        }
        else if(r == 1) {
            if(s.cmd != RET_SUCCESS) {
#ifdef PROX_DEBUG
                fprintf(stderr, "SocksClient: client << Error >> [%02x]\n", s.cmd);
#endif
                resetConnection(true);
                if(s.cmd == RET_UNREACHABLE)
                    setError(ErrHostNotFound);
                else if(s.cmd == RET_CONNREFUSED)
                    setError(ErrConnectionRefused);
                else
                    setError(ErrProxyNeg);
                return;
            }

#ifdef PROX_DEBUG
            fprintf(stderr, "SocksClient: client << Success >>\n");
#endif
            if(d->udp) {
                if(s.address_type == 0x03)
                    d->udpAddr = s.host;
                else
                    d->udpAddr = s.addr.toString();
                d->udpPort = s.port;
            }

            setOpenMode(QIODevice::ReadWrite);

            QPointer<QObject> self = this;
            setOpenMode(QIODevice::ReadWrite);
            emit connected();
            if(!self)
                return;

            if(!d->recvBuf.isEmpty()) {
                appendRead(d->recvBuf);
                d->recvBuf.resize(0);
                readyRead();
            }
        }
    }
}

void SocksClient::do_request()
{
#ifdef PROX_DEBUG
    fprintf(stderr, "SocksClient: Requesting ...\n");
#endif
    d->step = StepRequest;
    int cmd = d->udp ? REQ_UDPASSOCIATE : REQ_CONNECT;
    QByteArray buf;
    if(!d->real_host.isEmpty())
        buf = sp_set_request(d->real_host, d->real_port, cmd);
    else
        buf = sp_set_request(QHostAddress(), 0, cmd);
    writeData(buf);
}

void SocksClient::sock_bytesWritten(qint64 x)
{
    int bytes = x;
    if(d->pending >= bytes) {
        d->pending -= bytes;
        bytes = 0;
    }
    else {
        bytes -= d->pending;
        d->pending = 0;
    }
    if(bytes > 0)
        bytesWritten(bytes);
}

void SocksClient::sock_error(int x)
{
    if(isOpen()) {
        resetConnection();
        setError(ErrRead);
    }
    else {
        resetConnection(true);
        if(x == BSocket::ErrHostNotFound)
            setError(ErrProxyConnect);
        else if(x == BSocket::ErrConnectionRefused)
            setError(ErrProxyConnect);
        else if(x == BSocket::ErrRead)
            setError(ErrProxyNeg);
    }
}

void SocksClient::serve()
{
    d->waiting = false;
    d->step = StepVersion;
    continueIncoming();
}

void SocksClient::processIncoming(const QByteArray &block)
{
#ifdef PROX_DEBUG
    // show hex
    fprintf(stderr, "SocksClient: server recv { ");
    for(int n = 0; n < (int)block.size(); ++n)
        fprintf(stderr, "%02X ", (unsigned char)block[n]);
    fprintf(stderr, " } \n");
#endif
    d->recvBuf += block;

    if(!d->waiting)
        continueIncoming();
}

void SocksClient::continueIncoming()
{
    if(d->recvBuf.isEmpty())
        return;

    if(d->step == StepVersion) {
        SPCS_VERSION s;
        int r = spc_get_version(d->recvBuf, &s);
        if(r == -1) {
            resetConnection(true);
            setError(ErrProxyNeg);
            return;
        }
        else if(r == 1) {
            if(s.version != 0x05) {
                resetConnection(true);
                setError(ErrProxyNeg);
                return;
            }

            int methods = 0;
            for(int n = 0; n < (int)s.methodList.size(); ++n) {
                unsigned char c = s.methodList[n];
                if(c == 0x00)
                    methods |= AuthNone;
                else if(c == 0x02)
                    methods |= AuthUsername;
            }
            d->waiting = true;
            emit incomingMethods(methods);
        }
    }
    else if(d->step == StepAuth) {
        SPCS_AUTHUSERNAME s;
        int r = spc_get_authUsername(d->recvBuf, &s);
        if(r == -1) {
            resetConnection(true);
            setError(ErrProxyNeg);
            return;
        }
        else if(r == 1) {
            d->waiting = true;
            incomingAuth(s.user, s.pass);
        }
    }
    else if(d->step == StepRequest) {
        SPS_CONNREQ s;
        int r = sp_get_request(d->recvBuf, &s);
        if(r == -1) {
            resetConnection(true);
            setError(ErrProxyNeg);
            return;
        }
        else if(r == 1) {
            d->waiting = true;
            if(s.cmd == REQ_CONNECT) {
                if(!s.host.isEmpty())
                    d->rhost = s.host;
                else
                    d->rhost = s.addr.toString();
                d->rport = s.port;
                QIODevice::open(QIODevice::ReadWrite);
                incomingConnectRequest(d->rhost, d->rport);
            }
            else if(s.cmd == REQ_UDPASSOCIATE) {
                incomingUDPAssociateRequest();
            }
            else {
                requestDeny();
                return;
            }
        }
    }
}

void SocksClient::chooseMethod(int method)
{
    if(d->step != StepVersion || !d->waiting)
        return;

    unsigned char c;
    if(method == AuthNone) {
        d->step = StepRequest;
        c = 0x00;
    }
    else {
        d->step = StepAuth;
        c = 0x02;
    }

    // version response
    d->waiting = false;
    writeData(sps_set_version(c));
    continueIncoming();
}

void SocksClient::authGrant(bool b)
{
    if(d->step != StepAuth || !d->waiting)
        return;

    if(b)
        d->step = StepRequest;

    // auth response
    d->waiting = false;
    writeData(sps_set_authUsername(b));
    if(!b) {
        resetConnection(true);
        return;
    }
    continueIncoming();
}

void SocksClient::requestDeny()
{
    if(d->step != StepRequest || !d->waiting)
        return;

    // response
    d->waiting = false;
    writeData(sp_set_request(d->rhost, d->rport, RET_UNREACHABLE));
    resetConnection(true);
}

void SocksClient::grantConnect()
{
    if(d->step != StepRequest || !d->waiting)
        return;

    // response
    d->waiting = false;
    writeData(sp_set_request(d->rhost, d->rport, RET_SUCCESS));
    setOpenMode(QIODevice::ReadWrite);
#ifdef PROX_DEBUG
    fprintf(stderr, "SocksClient: server << Success >>\n");
#endif

    if(!d->recvBuf.isEmpty()) {
        appendRead(d->recvBuf);
        d->recvBuf.resize(0);
        readyRead();
    }
}

void SocksClient::grantUDPAssociate(const QString &relayHost, int relayPort)
{
    if(d->step != StepRequest || !d->waiting)
        return;

    // response
    d->waiting = false;
    writeData(sp_set_request(relayHost, relayPort, RET_SUCCESS));
    d->udp = true;
    setOpenMode(QIODevice::ReadWrite);
#ifdef PROX_DEBUG
    fprintf(stderr, "SocksClient: server << Success >>\n");
#endif

    if(!d->recvBuf.isEmpty())
        d->recvBuf.resize(0);
}

QHostAddress SocksClient::peerAddress() const
{
    return d->sock.peerAddress();
}

quint16 SocksClient::peerPort() const
{
    return d->sock.peerPort();
}

QString SocksClient::udpAddress() const
{
    return d->udpAddr;
}

quint16 SocksClient::udpPort() const
{
    return d->udpPort;
}

SocksUDP *SocksClient::createUDP(const QString &host, int port, const QHostAddress &routeAddr, int routePort)
{
    return new SocksUDP(this, host, port, routeAddr, routePort);
}

//----------------------------------------------------------------------------
// SocksServer
//----------------------------------------------------------------------------
class SocksServer::Private
{
public:
    QTcpServer          *serv = nullptr;
    QList<SocksClient*> incomingConns;
    QUdpSocket          *sd = nullptr;
};

SocksServer::SocksServer(QObject *parent)
:QObject(parent)
{
    d = new Private;
}

SocksServer::~SocksServer()
{
    stop();
    while (d->incomingConns.count()) {
        delete d->incomingConns.takeFirst();
    }
    delete d;
}

void SocksServer::setServerSocket(QTcpServer *server)
{
    d->serv = server;
    connect(d->serv, SIGNAL(newConnection()), SLOT(newConnection()));
}

bool SocksServer::isActive() const
{
    return d->serv->isListening();
}

bool SocksServer::listen(quint16 port, bool udp)
{
    stop();
    if (!d->serv) {
        setServerSocket(new QTcpServer(this));
    }
    if(!d->serv->listen(QHostAddress::Any, port))
        return false;
    if(udp) {
        d->sd = new QUdpSocket(this);
        if(!d->sd->bind(QHostAddress::LocalHost, port)) {
            delete d->sd;
            d->sd = nullptr;
            delete d->serv;
            d->serv = nullptr;
            return false;
        }
        connect(d->sd, SIGNAL(readyRead()), SLOT(sd_activated()));
    }
    return true;
}

void SocksServer::stop()
{
    delete d->sd;
    d->sd = nullptr;
    delete d->serv;
    d->serv = nullptr;
}

int SocksServer::port() const
{
    return d->serv? d->serv->serverPort(): 0;
}

QHostAddress SocksServer::address() const
{
    return d->serv? d->serv->serverAddress(): QHostAddress();
}

SocksClient *SocksServer::takeIncoming()
{
    if(d->incomingConns.isEmpty())
        return nullptr;

    SocksClient *c = d->incomingConns.takeFirst();

    // we don't care about errors anymore
    disconnect(c, SIGNAL(error(int)), this, SLOT(connectionError()));

    // don't serve the connection until the event loop, to give the caller a chance to map signals
    QTimer::singleShot(0, c, SLOT(serve()));

    return c;
}

void SocksServer::writeUDP(const QHostAddress &addr, int port, const QByteArray &data)
{
    if(d->sd) {
        d->sd->writeDatagram(data.data(), data.size(), addr, port);
    }
}

void SocksServer::newConnection()
{
    SocksClient *c = new SocksClient(d->serv->nextPendingConnection(), this);
    connect(c, SIGNAL(error(int)), this, SLOT(connectionError()));
    d->incomingConns.append(c);
    incomingReady();
}

void SocksServer::connectionError()
{
    SocksClient *c = static_cast<SocksClient *>(sender());
    d->incomingConns.removeAll(c);
    c->deleteLater();
}

void SocksServer::sd_activated()
{
    while (d->sd->hasPendingDatagrams()) {
        QByteArray datagram(d->sd->pendingDatagramSize(), Qt::Uninitialized);
        QHostAddress sender;
        quint16 senderPort;
        auto sz = d->sd->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        if (sz >= 0) {
            datagram.truncate(sz);
            incomingUDP(sender.toString(), senderPort, d->sd->peerAddress(), d->sd->peerPort(), datagram);
        }
    }
}

// CS_NAMESPACE_END
