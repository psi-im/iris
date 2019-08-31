/*
 * httpconnect.cpp - HTTP "CONNECT" proxy
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

#include "httpconnect.h"

#include "bsocket.h"

#include <QByteArray>
#include <QStringList>
#include <QtCrypto>

//#define PROX_DEBUG
#ifdef PROX_DEBUG
#include <stdio.h>
#endif

// CS_NAMESPACE_BEGIN
#ifdef PROX_DEBUG
QString escapeOutput(const QByteArray &in)
{
    QString out;
    for(int n = 0; n < in.size(); ++n) {
        if(in[n] == '\\') {
            out += QString("\\\\");
        }
        else if(in[n] >= 32 && in[n] < 127) {
            out += QChar::fromLatin1(in[n]);
        }
        else {
            out += QString().sprintf("\\x%02x", (unsigned char)in[n]);
        }
    }
    return out;
}
#endif

static QString extractLine(QByteArray *buf, bool *found)
{
    // Scan for newline
    int index = buf->indexOf ("\r\n");
    if (index == -1) {
        // Newline not found
        if (found)
            *found = false;
        return "";
    }
    else {
        // Found newline
        QString s = QString::fromLatin1(buf->left(index));
        buf->remove(0, index + 2);

        if (found)
            *found = true;
        return s;
    }
}

static bool extractMainHeader(const QString &line, QString *proto, int *code, QString *msg)
{
    int n = line.indexOf(' ');
    if(n == -1)
        return false;
    if(proto)
        *proto = line.mid(0, n);
    ++n;
    int n2 = line.indexOf(' ', n);
    if(n2 == -1)
        return false;
    if(code)
        *code = line.mid(n, n2-n).toInt();
    n = n2+1;
    if(msg)
        *msg = line.mid(n);
    return true;
}

class HttpConnect::Private
{
public:
    Private(HttpConnect *_q) :
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

    bool inHeader;
    QStringList headerLines;

    int toWrite;
    bool active;
};

HttpConnect::HttpConnect(QObject *parent)
:ByteStream(parent)
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

HttpConnect::~HttpConnect()
{
    resetConnection(true);
    delete d;
}

void HttpConnect::resetConnection(bool clear)
{
    if(d->sock.state() != BSocket::Idle)
        d->sock.close();
    if(clear) {
        clearReadBuffer();
        d->recvBuf.resize(0);
    }
    d->active = false;
    setOpenMode(QIODevice::NotOpen);
}

void HttpConnect::setAuth(const QString &user, const QString &pass)
{
    d->user = user;
    d->pass = pass;
}

void HttpConnect::connectToHost(const QString &proxyHost, int proxyPort, const QString &host, int port)
{
    resetConnection(true);

    d->host = proxyHost;
    d->port = proxyPort;
    d->real_host = host;
    d->real_port = port;

#ifdef PROX_DEBUG
    fprintf(stderr, "HttpConnect: Connecting to %s:%d", qPrintable(proxyHost), proxyPort);
    if(d->user.isEmpty())
        fprintf(stderr, "\n");
    else
        fprintf(stderr, ", auth {%s,%s}\n", qPrintable(d->user), qPrintable(d->pass));
#endif
    d->sock.connectToHost(d->host, d->port);
}

void HttpConnect::close()
{
    d->sock.close();
    if(d->sock.bytesToWrite() == 0)
        resetConnection();
}

qint64 HttpConnect::writeData(const char *data, qint64 maxSize)
{
    if(d->active)
        return d->sock.write(data, maxSize);
    return 0;
}

qint64 HttpConnect::bytesToWrite() const
{
    if(d->active)
        return d->sock.bytesToWrite();
    else
        return 0;
}

void HttpConnect::sock_connected()
{
#ifdef PROX_DEBUG
    fprintf(stderr, "HttpConnect: Connected\n");
#endif
    d->inHeader = true;
    d->headerLines.clear();

    // connected, now send the request
    QString s;
    s += QString("CONNECT ") + d->real_host + ':' + QString::number(d->real_port) + " HTTP/1.0\r\n";
    if(!d->user.isEmpty()) {
        QString str = d->user + ':' + d->pass;
        s += QString("Proxy-Authorization: Basic ") + QCA::Base64().encodeString(str) + "\r\n";
    }
    s += "Pragma: no-cache\r\n";
    s += "\r\n";

    QByteArray block = s.toUtf8();
#ifdef PROX_DEBUG
    fprintf(stderr, "HttpConnect: writing: {%s}\n", qPrintable(escapeOutput(block)));
#endif
    d->toWrite = block.size();
    d->sock.write(block);
}

void HttpConnect::sock_connectionClosed()
{
    if(d->active) {
        resetConnection();
        connectionClosed();
    }
    else {
        setError(ErrProxyNeg);
    }
}

void HttpConnect::sock_delayedCloseFinished()
{
    if(d->active) {
        resetConnection();
        delayedCloseFinished();
    }
}

void HttpConnect::sock_readyRead()
{
    QByteArray block = d->sock.readAll();

    if(!d->active) {
        d->recvBuf += block;

        if(d->inHeader) {
            // grab available lines
            while(1) {
                bool found;
                QString line = extractLine(&d->recvBuf, &found);
                if(!found)
                    break;
                if(line.isEmpty()) {
                    d->inHeader = false;
                    break;
                }
                d->headerLines += line;
            }

            // done with grabbing the header?
            if(!d->inHeader) {
                QString str = d->headerLines.first();
                d->headerLines.takeFirst();

                QString proto;
                int code;
                QString msg;
                if(!extractMainHeader(str, &proto, &code, &msg)) {
#ifdef PROX_DEBUG
                    fprintf(stderr, "HttpConnect: invalid header!\n");
#endif
                    resetConnection(true);
                    setError(ErrProxyNeg);
                    return;
                }
                else {
#ifdef PROX_DEBUG
                    fprintf(stderr, "HttpConnect: header proto=[%s] code=[%d] msg=[%s]\n", qPrintable(proto), code, qPrintable(msg));
                    for(QStringList::ConstIterator it = d->headerLines.begin(); it != d->headerLines.end(); ++it)
                        fprintf(stderr, "HttpConnect: * [%s]\n", qPrintable(*it));
#endif
                }

                if(code == 200) { // OK
#ifdef PROX_DEBUG
                    fprintf(stderr, "HttpConnect: << Success >>\n");
#endif
                    d->active = true;
                    setOpenMode(QIODevice::ReadWrite);
                    connected();

                    if(!d->recvBuf.isEmpty()) {
                        appendRead(d->recvBuf);
                        d->recvBuf.resize(0);
                        readyRead();
                        return;
                    }
                }
                else {
                    int err;
                    QString errStr;
                    if(code == 407) { // Authentication failed
                        err = ErrProxyAuth;
                        errStr = tr("Authentication failed");
                    }
                    else if(code == 404) { // Host not found
                        err = ErrHostNotFound;
                        errStr = tr("Host not found");
                    }
                    else if(code == 403) { // Access denied
                        err = ErrProxyNeg;
                        errStr = tr("Access denied");
                    }
                    else if(code == 503) { // Connection refused
                        err = ErrConnectionRefused;
                        errStr = tr("Connection refused");
                    }
                    else { // invalid reply
                        err = ErrProxyNeg;
                        errStr = tr("Invalid reply");
                    }

#ifdef PROX_DEBUG
                    fprintf(stderr, "HttpConnect: << Error >> [%s]\n", qPrintable(errStr));
#endif
                    resetConnection(true);
                    setError(err);
                    return;
                }
            }
        }
    }
    else {
        appendRead(block);
        readyRead();
        return;
    }
}

void HttpConnect::sock_bytesWritten(qint64 x)
{
    if(d->toWrite > 0) {
        int size = x;
        if(d->toWrite < x)
            size = d->toWrite;
        d->toWrite -= size;
        x -= size;
    }

    if(d->active && x > 0)
        bytesWritten(x);
}

void HttpConnect::sock_error(int x)
{
    if(d->active) {
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

// CS_NAMESPACE_END
