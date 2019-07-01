/*
 * s5b.h - direct connection protocol via tcp
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

#ifndef XMPP_S5B_H
#define XMPP_S5B_H

#include <QObject>
#include <QList>
#include <QHostAddress>
#include <tcpportreserver.h>

#include "bytestream.h"
#include "xmpp_bytestream.h"
#include "xmpp/jid/jid.h"
#include "xmpp_task.h"
#include "xmpp_stanza.h"

class SocksClient;
class SocksUDP;

namespace XMPP
{
    namespace Jingle {
        namespace S5B {
            class Manager;
        }
    }

    class StreamHost;
    class Client;
    class TcpPortReserver;
    class S5BConnection;
    class S5BManager;
    class JT_PushS5B;
    struct S5BRequest;
    typedef QList<StreamHost> StreamHostList;
    typedef QList<S5BConnection*> S5BConnectionList;

    class S5BDatagram
    {
    public:
        S5BDatagram();
        S5BDatagram(int source, int dest, const QByteArray &data);

        int sourcePort() const;
        int destPort() const;
        QByteArray data() const;

    private:
        int _source = 0;
        int _dest = 0;
        QByteArray _buf;
    };

    class S5BConnection : public BSConnection
    {
        Q_OBJECT
    public:
        enum Mode { Stream, Datagram };

        ~S5BConnection();

        Jid proxy() const;
        void setProxy(const Jid &proxy);

        void connectToJid(const Jid &peer, const QString &sid, Mode m = Stream);
        void connectToJid(const Jid &peer, const QString &sid) {
            connectToJid(peer, sid, Stream);
        }

        void accept();
        void close();

        Jid peer() const;
        QString sid() const;
        BytestreamManager* manager() const;
        bool isRemote() const;
        Mode mode() const;
        int state() const;

        qint64 bytesAvailable() const;
        qint64 bytesToWrite() const;

        void writeDatagram(const S5BDatagram &);
        S5BDatagram readDatagram();
        int datagramsAvailable() const;

    protected:
        qint64 writeData(const char *data, qint64 maxSize);
        qint64 readData(char * data, qint64 maxSize);

    signals:
        void proxyQuery();                             // querying proxy for streamhost information
        void proxyResult(bool b);                      // query success / fail
        void requesting();                             // sent actual S5B request (requester only)
        void accepted();                               // target accepted (requester only
        void tryingHosts(const StreamHostList &hosts); // currently connecting to these hosts
        void proxyConnect();                           // connecting to proxy
        void waitingForActivation();                   // waiting for activation (target only)
        void connected();                              // connection active
        void datagramReady();

    private slots:
        void doPending();

        void sc_connectionClosed();
        void sc_delayedCloseFinished();
        void sc_readyRead();
        void sc_bytesWritten(qint64);
        void sc_error(int);

        void su_packetReady(const QByteArray &buf);

    private:
        class Private;
        Private *d;

        void resetConnection(bool clear=false);
        void handleUDP(const QByteArray &buf);
        void sendUDP(const QByteArray &buf);

        friend class S5BManager;
        void man_waitForAccept(const S5BRequest &r);
        void man_clientReady(SocksClient *, SocksUDP *);
        void man_udpReady(const QByteArray &buf);
        void man_failed(int);
        S5BConnection(S5BManager *, QObject *parent=nullptr);
    };

    class S5BManager : public BytestreamManager
    {
        Q_OBJECT
    public:
        S5BManager(Client *);
        ~S5BManager();

        static const char* ns();
        Client *client() const;
        JT_PushS5B *jtPush() const;

        bool isAcceptableSID(const Jid &peer, const QString &sid) const;

        BSConnection *createConnection();
        S5BConnection *takeIncoming();

        class Item;
        class Entry;

    protected:
        const char* sidPrefix() const;

    private slots:
        void ps_incoming(const S5BRequest &req);
        void ps_incomingUDPSuccess(const Jid &from, const QString &dstaddr);
        void ps_incomingActivate(const Jid &from, const QString &sid, const Jid &streamHost);
        void item_accepted();
        void item_tryingHosts(const StreamHostList &list);
        void item_proxyConnect();
        void item_waitingForActivation();
        void item_connected();
        void item_error(int);
        void query_finished();

    private:
        class Private;
        Private *d;

        S5BConnection *findIncoming(const Jid &from, const QString &sid) const;
        Entry *findEntry(S5BConnection *) const;
        Entry *findEntry(Item *) const;
        Entry *findEntryByHash(const QString &key) const;
        Entry *findEntryBySID(const Jid &peer, const QString &sid) const;

        void entryContinue(Entry *e);
        void queryProxy(Entry *e);
        bool targetShouldOfferProxy(Entry *e);

        friend class S5BConnection;
        void con_connect(S5BConnection *);
        void con_accept(S5BConnection *);
        void con_reject(S5BConnection *);
        void con_unlink(S5BConnection *);
        void con_sendUDP(S5BConnection *, const QByteArray &buf);

        friend class S5BServer;
        bool srv_ownsHash(const QString &key) const;
        void srv_incomingReady(SocksClient *sc, const QString &key);
        void srv_incomingUDP(bool init, const QHostAddress &addr, int port, const QString &key, const QByteArray &data);

        friend class Item;
        void doSuccess(const Jid &peer, const QString &id, const Jid &streamHost);
        void doError(const Jid &peer, const QString &id, Stanza::Error::ErrorCond, const QString &);
        void doActivate(const Jid &peer, const QString &sid, const Jid &streamHost);
    };

    class S5BConnector : public QObject
    {
        Q_OBJECT
    public:
        S5BConnector(QObject *parent=nullptr);
        ~S5BConnector();

        void resetConnection();
        void start(const Jid &self, const StreamHostList &hosts, const QString &key, bool udp, int timeout);
        SocksClient *takeClient();
        SocksUDP *takeUDP();
        StreamHost streamHostUsed() const;

        class Item;

    signals:
        void result(bool);

    private slots:
        void item_result(bool);
        void t_timeout();

    private:
        class Private;
        Private *d;

        friend class S5BManager;
        void man_udpSuccess(const Jid &streamHost);
    };


    class S5BServersProducer : public TcpPortScope
    {
    protected:
        TcpPortServer* makeServer(QTcpServer *socket); // in fact returns S5BServer
    };

    class S5BServer : public TcpPortServer
    {
        Q_OBJECT

    public:
        S5BServer(QTcpServer *serverSocket);
        ~S5BServer();

        void writeUDP(const QHostAddress &addr, int port, const QByteArray &data);
        bool isActive() const;
        bool hasKey(const QString &key);
        void registerKey(const QString &key);
        void unregisterKey(const QString &key);

    signals:
        void incomingConnection(SocksClient *c, const QString &key);
        void incomingUdp(bool isInit, const QHostAddress &addr, int sourcePort, const QString &key, const QByteArray &data);

    private:
        class Private;
        QScopedPointer<Private> d;
    };

    class JT_S5B : public Task
    {
        Q_OBJECT
    public:
        JT_S5B(Task *);
        ~JT_S5B();

        void request(const Jid &to, const QString &sid, const QString &dstaddr,
                     const StreamHostList &hosts, bool fast, bool udp=false);
        void requestProxyInfo(const Jid &to);
        void requestActivation(const Jid &to, const QString &sid, const Jid &target);

        void onGo();
        void onDisconnect();
        bool take(const QDomElement &);

        Jid streamHostUsed() const;
        StreamHost proxyInfo() const;

    private slots:
        void t_timeout();

    private:
        class Private;
        Private *d;
    };

    struct S5BRequest
    {
        Jid from;
        QString id, sid, dstaddr;
        StreamHostList hosts;
        bool fast;
        bool udp;
    };
    class JT_PushS5B : public Task
    {
        Q_OBJECT
    public:
        JT_PushS5B(Task *);
        ~JT_PushS5B();

        int priority() const;

        void respondSuccess(const Jid &to, const QString &id, const Jid &streamHost);
        void respondError(const Jid &to, const QString &id,
                          Stanza::Error::ErrorCond cond, const QString &str);
        void sendUDPSuccess(const Jid &to, const QString &dstaddr);
        void sendActivate(const Jid &to, const QString &sid, const Jid &streamHost);

        bool take(const QDomElement &);

    signals:
        void incoming(const S5BRequest &req);
        void incomingUDPSuccess(const Jid &from, const QString &dstaddr);
        void incomingActivate(const Jid &from, const QString &sid, const Jid &streamHost);
    };

    class StreamHost
    {
    public:
        StreamHost();

        const Jid & jid() const;
        const QString & host() const;
        int port() const;
        bool isProxy() const;
        void setJid(const Jid &);
        void setHost(const QString &);
        void setPort(int);
        void setIsProxy(bool);

    private:
        Jid j;
        QString v_host;
        int v_port;
        bool proxy;
    };
}

#endif
