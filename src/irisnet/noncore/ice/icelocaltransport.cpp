/*
 * Copyright (C) 2009-2010  Barracuda Networks, Inc.
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "icelocaltransport.h"

#include "iceagent.h"
#include "icecandidate.h"
#include "objectsession.h"
#include "stunallocate.h"
#include "stunbinding.h"
#include "stunmessage.h"
#include "stuntransaction.h"
#include "turnclient.h"

#include <QHostAddress>
#include <QTimer>
#include <QUdpSocket>
#include <QtCrypto>

// don't queue more incoming packets than this per transmit path
#define MAX_PACKET_QUEUE 64

namespace XMPP::ICE {
enum { Direct, Relayed };

//----------------------------------------------------------------------------
// SafeUdpSocket
//----------------------------------------------------------------------------
// DOR-safe wrapper for QUdpSocket
class SafeUdpSocket : public QObject {
    Q_OBJECT

private:
    ObjectSession sess;
    QUdpSocket   *sock;
    int           writtenCount;

public:
    SafeUdpSocket(QUdpSocket *_sock, QObject *parent = nullptr) : QObject(parent), sess(this), sock(_sock)
    {
        sock->setParent(this);
        connect(sock, &QUdpSocket::readyRead, this, &SafeUdpSocket::sock_readyRead);
        connect(sock, &QUdpSocket::bytesWritten, this, &SafeUdpSocket::sock_bytesWritten);

        writtenCount = 0;
    }

    ~SafeUdpSocket()
    {
        if (sock) {
            QUdpSocket *out = release();
            out->deleteLater();
        }
    }

    QUdpSocket *release()
    {
        sock->disconnect(this);
        sock->setParent(nullptr);
        QUdpSocket *out = sock;
        sock            = nullptr;
        return out;
    }

    TransportAddress localTransportAddress() const { return { sock->localAddress(), sock->localPort() }; }

    QHostAddress localAddress() const { return sock->localAddress(); }

    quint16 localPort() const { return sock->localPort(); }

    bool hasPendingDatagrams() const { return sock->hasPendingDatagrams(); }

    QByteArray readDatagram(TransportAddress &address)
    {
        if (!sock->hasPendingDatagrams())
            return QByteArray();

        QByteArray buf;
        buf.resize(int(sock->pendingDatagramSize()));
        sock->readDatagram(buf.data(), buf.size(), &address.addr, &address.port);
        return buf;
    }

    void writeDatagram(const QByteArray &buf, const TransportAddress &address)
    {
        sock->writeDatagram(buf, address.addr, address.port);
    }

signals:
    void readyRead();
    void datagramsWritten(int count);

private slots:
    void sock_readyRead() { emit readyRead(); }

    void sock_bytesWritten(qint64 bytes)
    {
        Q_UNUSED(bytes);

        ++writtenCount;
        sess.deferExclusive(this, "processWritten");
    }

    void processWritten()
    {
        int count    = writtenCount;
        writtenCount = 0;

        emit datagramsWritten(count);
    }
};

//----------------------------------------------------------------------------
// LocalTransport
//----------------------------------------------------------------------------
class LocalTransport::Private : public QObject {
    Q_OBJECT

public:
    class WriteItem {
    public:
        enum Type { Direct, Pool, Turn };

        Type             type;
        TransportAddress addr;
    };

    class Written {
    public:
        TransportAddress addr;
        int              count;
    };

    class Datagram {
    public:
        TransportAddress addr;
        QByteArray       buf;
    };

    using StunServer  = AbstractStunDisco::Service::Ptr;
    using StunServers = QList<StunServer>;

    struct RecoveringTurn {
        StunServer  server;
        TurnClient *client     = nullptr;
        int         retryCount = 0;
    };

    enum class State { None, Starting, Active, Stopping, Stopped };

    LocalTransport             *q;
    ObjectSession               sess;
    QUdpSocket                 *extSock = nullptr;
    SafeUdpSocket              *sock    = nullptr;
    StunTransactionPool::Ptr    pool;
    std::vector<RecoveringTurn> turnClients;
    std::vector<StunBinding *>  stunClients;
    AbstractStunDisco          *stunDiscoverer = nullptr;
    // TransportAddress            addr;
    LocalAddress localAddress;
    QHostAddress extAddr; // either configured or server reflexive address
    // TransportAddress         refAddr;
    // TransportAddress         relAddr;
    // QHostAddress             refAddrSource;

    // TransportAddress  stunBindAddr;
    // TransportAddress  stunRelayAddr;
    QList<StunServer> pendingStuns;

    QString          clientSoftware;
    QList<Datagram>  in;
    QList<Datagram>  inRelayed;
    QList<WriteItem> pendingWrites;

    State state             = State::None;
    bool  gatheringComplete = false;
    bool  borrowedSocket    = false; // where extSock is borrowed from a port reserver
    int   debugLevel        = Transport::DL_None;

    Private(LocalTransport *_q) : QObject(_q), q(_q), sess(this) { }

    ~Private() { reset(); }

    void reset()
    {
        sess.reset();

        for (auto s : stunClients)
            disconnect(s);
        qDeleteAll(stunClients);
        stunClients.clear();
        for (auto t : turnClients)
            disconnect(t.client);
        qDeleteAll(turnClients);
        turnClients.clear();

        if (sock) { // if started
            if (extSock) {
                sock->release(); // detaches the socket but doesn't destroy
                extSock = nullptr;
            }

            delete sock;
            sock = nullptr;
        }

        addr = TransportAddress();
        pendingStuns.clear();

        in.clear();
        inRelayed.clear();
        pendingWrites.clear();

        state = State::None;
    }

    void start()
    {
        Q_ASSERT(!sock);
        if (state >= State::Starting)
            return;
        state = State::Starting;
        QTimer::singleShot(0, this, [this]() {
            if (state >= State::Stopping)
                return;

            if (extSock) {
                sock = new SafeUdpSocket(extSock, this);
            } else {
                QUdpSocket *qsock = createSocket(); // will be bound to whatever is passed to q->start()
                if (!qsock) {
                    // signal emitted in this case.  bail
                    return;
                }

                sock = new SafeUdpSocket(qsock, this);
            }

            prepareSocket();

            pool = StunTransactionPool::Ptr::create(StunTransaction::Udp);
            pool->setDebugLevel((StunTransactionPool::DebugLevel)debugLevel);
            connect(pool.data(), &StunTransactionPool::outgoingMessage, this, &Private::pool_outgoingMessage);
            connect(pool.data(), &StunTransactionPool::needAuthParams, this, &Private::pool_needAuthParams);
            connect(pool.data(), &StunTransactionPool::debugLine, this,
                    [this](const QString &line) { emit q->debugLine(line); });
            pool->setLongTermAuthEnabled(true);

            for (auto const &s : pendingStuns) {
                if (isAcceptableService(s)) {
                    initExternalService(s);
                }
            }
            pendingStuns = {};

            state = State::Active;
            emit q->started();
        });
    }

    void stop()
    {
        Q_ASSERT(sock);
        if (state >= State::Stopping) {
            emit q->debugLine(QString("local transport %1 is already stopping. just wait...").arg(addr));
            return;
        } else {
            emit q->debugLine(QString("stopping local transport %1.").arg(addr));
        }

        state = State::Stopping;

        if (stunDiscoverer) {
            stunDiscoverer->disconnect(this);
            stunDiscoverer = nullptr;
        }
        for (auto s : stunClients)
            disconnect(s);
        qDeleteAll(stunClients);
        stunClients.clear();
        if (turnClients.empty()) {
            sess.defer(this, "postStop");
            return;
        }
        for (auto &t : turnClients)
            t.client->close(); // will emit closed()
    }

    void setStunDiscoverer(AbstractStunDisco *discoverer)
    {
        stunDiscoverer = discoverer;
        connect(discoverer, &AbstractStunDisco::serviceAdded, this, [this](StunServer service) {
            if (state <= State::Starting)
                pendingStuns.append(service);
            else if (state < State::Stopping && isAcceptableService(service))
                initExternalService(service);
        });
        connect(discoverer, &AbstractStunDisco::serviceModified, this, [this](AbstractStunDisco::Service::Ptr) {});
        connect(discoverer, &AbstractStunDisco::serviceRemoved, this, [this](AbstractStunDisco::Service::Ptr) {});
        connect(discoverer, &AbstractStunDisco::discoFinished, this, [this]() {});
    }

    QUdpSocket *takeBorrowedSocket()
    {
        if (borrowedSocket) {
            extSock->disconnect(this);
            auto r         = extSock;
            borrowedSocket = false;
            return r;
        }
    }

private:
    // note: emits signal on error
    QUdpSocket *createSocket()
    {
        QUdpSocket *qsock = new QUdpSocket(this);
        if (!qsock->bind(addr.addr, 0)) {
            delete qsock;
            emit q->error(LocalTransport::ErrorBind);
            return nullptr;
        }

        return qsock;
    }

    void prepareSocket()
    {
        // addr = sock->localTransportAddress();

        connect(sock, &SafeUdpSocket::readyRead, this, &Private::sock_readyRead);
        connect(sock, &SafeUdpSocket::datagramsWritten, this, &Private::sock_datagramsWritten);
    }

    /*
     * Hadles TURN Allocate Mismatch error (3 attempts to sucessfully connect)
     * return true if we are retrying, false if we should error out
     */
    bool handleAllocateMismatch(RecoveringTurn &turn)
    {
        // don't allow retrying if activated or stopping)
        if (turn.client->isActivated() || state >= State::Stopping)
            return false;

        ++turn.retryCount;
        if (turn.retryCount < 3) {
            if (debugLevel >= Transport::DL_Info)
                emit q->debugLine("retrying...");

            delete sock;
            sock = nullptr;

            QUdpSocket *qsock = createSocket();
            if (!qsock) {
                // signal emitted in this case.  bail.
                //   (return true so caller takes no action)
                return true;
            }

            sock = new SafeUdpSocket(qsock, this);
            prepareSocket();

            return true;
        }

        return false;
    }

    /**
     * Process data coming from a stun/turn server (not peer)
     *
     * return true if data packet, false if pool or nothing
     */
    bool processIncomingStun(const QByteArray &buf, const TransportAddress &fromAddr, Datagram *dg)
    {
        QByteArray       data;
        TransportAddress dataAddr;

        bool notStun;
        if (pool->writeIncomingMessage(buf, &notStun, fromAddr))
            return false; // handled as stun transaction. no data
        // turn servers use the same pool. So if the incomming packet is from a tern server it has to be a data packet
        for (auto const &rt : turnClients) {
            if (!rt.client->isUdp())
                continue;
            data = rt.client->processIncomingDatagram(buf, notStun, dataAddr);
            if (!data.isNull()) {
                dg->addr = dataAddr;
                dg->buf  = data;
                return true;
            }
        }
        if (debugLevel >= Transport::DL_Packet)
            emit q->debugLine(
                "Warning: server responded with what doesn't seem to be a STUN or data packet, skipping.");

        return false;
    }

    // called when one of external services was deleted
    void onExtServiceFinished()
    {
        if (!stunClients.empty() || !turnClients.empty())
            return;
        gatheringComplete = true;
        if (state == State::Stopping) {
            reset();
            state = State::Stopped;
            emit q->stopped();
            return;
        }
    }

    void postStop()
    {
        reset();
        emit q->stopped();
    }

    void addTcpTurn(AbstractStunDisco::Service::Ptr service, const QHostAddress &addr)
    {
        auto turn = std::make_shared<TurnClient>();
        turn->setDebugLevel(TurnClient::DebugLevel(debugLevel));
        connect(turn.get(), &TurnClient::activated, this, [this, turn]() {
            auto allocate = turn->stunAllocate();
            auto refAddr  = allocate->reflexiveAddress();
            if (debugLevel >= Transport::DL_Info)
                emit q->debugLine(QLatin1String("Server says we are ") + refAddr);
            auto relayAddr = allocate->relayedAddress();
            if (debugLevel >= Transport::DL_Info)
                emit q->debugLine(QLatin1String("Server relays via ") + relayAddr);

            auto ci        = CandidateInfo::make();
            ci->addr       = relayAddr;
            ci->related    = refAddr;
            ci->type       = RelayedType;
            ci->base       = ci->addr;
            ci->network    = localAddress.network;
            ci->foundation = Agent::instance()->foundation(RelayedType, ci->base.addr, turn->serverAddress().addr,
                                                           QAbstractSocket::TcpSocket);
            emit q->candidateFound(ci);
        });
        connect(turn.get(), &TurnClient::closed, this, [this, turn]() {
            ObjectSessionWatcher watch(&sess);
            emit                 q->candidateClosed(turn);
            if (!watch.isValid())
                return;

            turn->disconnect(this);
        });
        connect(turn.get(), &TurnClient::error, this, [this, turn](XMPP::TurnClient::Error e) {
            Q_UNUSED(e)

            ObjectSessionWatcher watch(&sess);

            removeLocalCandidates(turn);
            if (!watch.isValid())
                return;

            turn->disconnect(this);
        });
        connect(turn.get(), &TurnClient::debugLine, this, [this](const QString &line) { emit q->debugLine(line); });
        turn->setClientSoftwareNameAndVersion(clientSoftware);
        turn->setProxy(proxy);
        turn->setUsername(service->username);
        turn->setPassword(service->password);
        auto taddr = TransportAddress { addr, service->port };
        turn->start(taddr);
        tcpTurn.append(turn);

        emit q->debugLine(QLatin1String("starting TURN transport with server ") + taddr
                          + QLatin1String(" for component ") + QString::number(id));
    }

    bool isAcceptableService(AbstractStunDisco::Service::Ptr srv) const
    {
        Q_ASSERT(sock != nullptr);
        return !(sock->localAddress().protocol() == QAbstractSocket::IPv4Protocol ? srv->addresses4 : srv->addresses6)
                    .isEmpty()
            && (srv->transport == AbstractStunDisco::Tcp || !(srv->flags & AbstractStunDisco::Tls));
        // TODO support STUN over DTLS
    }

    void onServerReflexiveFound(const TransportAddress &rflxAddr, const QHostAddress &sourceAddr)
    {
        auto ci     = CandidateInfo::make();
        ci->addr    = rflxAddr;
        ci->base    = { sock->localAddress(), sock->localPort() };
        ci->related = ci->base;
        ci->type    = ServerReflexiveType;
        // ci->componentId = id;
        // ci->priority    = choose_default_priority(ci->type, 65535 - addrAt, lt->isVpn, ci->componentId);
        ci->network = localAddress.network;
        ci->foundation
            = Agent::instance()->foundation(ServerReflexiveType, ci->base.addr, sourceAddr, QAbstractSocket::UdpSocket);
        emit q->candidateFound(ci);
    }

    void addStun(StunServer service)
    {
        auto binding = new StunBinding(pool.data());
        connect(binding, &StunBinding::success, this, [this, binding]() {
            auto refAddr       = binding->reflexiveAddress();
            auto refAddrSource = binding->stunAddress().addr;
            delete binding;
            onServerReflexiveFound(refAddr, refAddrSource);
        });
        connect(binding, &StunBinding::error, this, [this, binding](XMPP::StunBinding::Error err) {
            emit q->debugLine(QString("stun bind failed: ") + QString::number(int(err)));
            delete binding;
        });
        connect(binding, &QObject::destroyed, this, [this, binding]() {
            auto it = std::find(stunClients.begin(), stunClients.end(), binding);
            stunClients.erase(it);
            onExtServiceFinished();
        });
        auto &addresses = sock->localAddress().protocol() == QAbstractSocket::IPv4Protocol ? service->addresses4
                                                                                           : service->addresses6;
        stunClients.push_back(binding);
        binding->start({ addresses[0], service->port });
    }

    void addTurn(StunServer service)
    {
        if (!service->username.isEmpty()) {
            pool->setUsername(service->username);
            pool->setPassword(service->password);
        }

        auto turn = new TurnClient(this);
        turn->setDebugLevel((TurnClient::DebugLevel)debugLevel);
        connect(turn, &TurnClient::connected, this, [this, turn]() {
            if (debugLevel >= IceTransport::DL_Info)
                emit q->debugLine(QString(turn->serverAddress()) + QLatin1String(" turn_connected"));
        });
        connect(turn, &TurnClient::tlsHandshaken, this, [this, turn]() {
            if (debugLevel >= IceTransport::DL_Info)
                emit q->debugLine(QString(turn->serverAddress()) + QLatin1String(" turn_tlsHandshaken"));
        });
        connect(turn, &TurnClient::closed, this, [this, turn]() {
            if (debugLevel >= IceTransport::DL_Info)
                emit q->debugLine(QString(turn->serverAddress()) + QLatin1String(" turn_closed"));
            delete turn;
        });
        connect(turn, &TurnClient::activated, this, [this]() {
            StunAllocate *allocate = turnClients->stunAllocate();

            // take reflexive address from TURN only if we are not using a
            //   separate STUN server
            if (!stunBindAddr.isValid() || stunBindAddr == stunRelayAddr) {
                refAddr       = allocate->reflexiveAddress();
                refAddrSource = stunRelayAddr.addr;
            }

            if (debugLevel >= IceTransport::DL_Info)
                emit q->debugLine(QLatin1String("Server says we are ") + allocate->reflexiveAddress());

            relAddr = allocate->relayedAddress();
            if (debugLevel >= IceTransport::DL_Info)
                emit q->debugLine(QLatin1String("Server relays via ") + relAddr);

            emit q->addressesChanged();
        });
        connect(turn, &TurnClient::packetsWritten, this, [this](int count, const XMPP::TransportAddress &addr) {
            emit q->datagramsWritten(Relayed, count, addr);
        });
        connect(turn, &TurnClient::error, this, [this, turn](XMPP::TurnClient::Error e) {
            if (debugLevel >= IceTransport::DL_Info)
                emit q->debugLine(QString(turn->serverAddress()) + QString(" turn_error: ") + turn->errorString());
            if (e == TurnClient::ErrorMismatch && !extSock && handleAllocateMismatch(*findTurn(turn)))
                return;
            delete turn;
        });
        connect(turn, &TurnClient::outgoingDatagram, this, [this](const QByteArray &buf) {
            WriteItem wi;
            wi.type = WriteItem::Turn;
            pendingWrites += wi;
            sock->writeDatagram(buf, stunRelayAddr);
        });
        connect(turn, &TurnClient::debugLine, this,
                [this, turn](const QString &line) { emit q->debugLine(QString(turn->serverAddress()) + ' ' + line); });
        connect(turn, &QObject::destroyed, this, [this]() mutable {
            auto it = findTurn(sender());
            turnClients.erase(it);
            onExtServiceFinished();
        });
        turn->setClientSoftwareNameAndVersion(clientSoftware);
        turnClients.push_back({ service, turn, 0 });

        auto &addresses = sock->localAddress().protocol() == QAbstractSocket::IPv4Protocol ? service->addresses4
                                                                                           : service->addresses6;
        turn->connectToHost(pool.data(), { addresses[0], service->port });
    }

    void initExternalService(AbstractStunDisco::Service::Ptr service)
    {
        if (service->flags & AbstractStunDisco::Relay)
            addStun(service);
        else
            addTurn(service);
    }

    void sock_readyRead()
    {
        ObjectSessionWatcher watch(&sess);

        QList<Datagram> dreads; // direct
        QList<Datagram> rreads; // relayed

        while (sock->hasPendingDatagrams()) {
            TransportAddress from;
            Datagram         dg;

            QByteArray buf = sock->readDatagram(from); // got raw udp payload
            if (buf.isEmpty())                         // it's weird we ever came here, but should relax static analyzer
                break;
            qDebug("got packet from %s", qPrintable(from));
            bool isFromStun = std::any_of(stunClients.begin(), stunClients.end(),
                                          [&from](auto const &i) { return i->stunAddress() == from; });
            if (!isFromStun)
                isFromStun = std::any_of(turnClients.begin(), turnClients.end(),
                                         [&from](auto const &i) { return i->serverAddress() == from; });

            if (isFromStun) {
                bool haveData = processIncomingStun(buf, from, &dg);

                // processIncomingStun could cause signals to
                //   emit.  for example, stopped()
                if (!watch.isValid())
                    return;

                if (haveData)
                    rreads += dg;
            } else {
                dg.addr = from;
                dg.buf  = buf;
                dreads += dg;
            }
        }

        if (dreads.count() > 0) {
            in += dreads;
            emit q->readyRead(Direct);
            if (!watch.isValid())
                return;
        }

        if (rreads.count() > 0) {
            inRelayed += rreads;
            emit q->readyRead(Relayed);
        }
    }

    void sock_datagramsWritten(int count)
    {
        QList<Written> dwrites;
        int            twrites = 0;

        while (count > 0) {
            Q_ASSERT(!pendingWrites.isEmpty());
            WriteItem wi = pendingWrites.takeFirst();
            --count;

            if (wi.type == WriteItem::Direct) {
                int at = -1;
                for (int n = 0; n < dwrites.count(); ++n) {
                    if (dwrites[n].addr == wi.addr) {
                        at = n;
                        break;
                    }
                }

                if (at != -1) {
                    ++dwrites[at].count;
                } else {
                    Written wr;
                    wr.addr  = wi.addr;
                    wr.count = 1;
                    dwrites += wr;
                }
            } else if (wi.type == WriteItem::Turn)
                ++twrites;
        }

        if (dwrites.isEmpty() && twrites == 0)
            return;

        ObjectSessionWatcher watch(&sess);

        if (!dwrites.isEmpty()) {
            for (const Written &wr : qAsConst(dwrites)) {
                emit q->datagramsWritten(Direct, wr.count, wr.addr);
                if (!watch.isValid())
                    return;
            }
        }

        if (twrites > 0) {
            // note: this will invoke turn_packetsWritten()
            turnClients->outgoingDatagramsWritten(twrites);
        }
    }

    void pool_outgoingMessage(const QByteArray &packet, const XMPP::TransportAddress &toAddress)
    {
        // warning: read StunTransactionPool docs before modifying
        //   this function

        WriteItem wi;
        wi.type = WriteItem::Pool;
        pendingWrites += wi;
        // emit q->debugLine(QString("Sending udp packet from: %1:%2 to: %3:%4")
        //                      .arg(sock->localAddress().toString())
        //                      .arg(sock->localPort())
        //                      .arg(toAddress.toString())
        //                      .arg(toPort));

        sock->writeDatagram(packet, toAddress);
    }

    void pool_needAuthParams(const XMPP::TransportAddress &addr)
    {
        // we can get this signal if the user did not provide
        //   creds to us.  however, since this class doesn't support
        //   prompting just continue on as if we had a blank
        //   user/pass
        pool->continueAfterParams(addr);
    }

    inline std::vector<RecoveringTurn>::iterator findTurn(QObject *client)
    {
        return std::find_if(turnClients.begin(), turnClients.end(),
                            [client](auto const &r) { return r.client == client; });
    }
};

LocalTransport::LocalTransport(QObject *parent) : Transport(parent) { d = new Private(this); }

LocalTransport::~LocalTransport() { delete d; }

void LocalTransport::setSocket(QUdpSocket *socket, bool borrowedSocket, const LocalAddress &la)
{
    d->extSock        = socket;
    d->borrowedSocket = socket ? borrowedSocket : false;
    d->localAddress   = la;
}

QUdpSocket *LocalTransport::takeBorrowedSocket() { return d->takeBorrowedSocket(); }

QNetworkInterface::InterfaceType LocalTransport::networkType() const { return d->localAddress.type; }

const QHostAddress &LocalTransport::externalAddress() const { return d->extAddr; }

void LocalTransport::setExternalAddress(const QHostAddress &addr) { d->extAddr = addr; }

void LocalTransport::setClientSoftwareNameAndVersion(const QString &str) { d->clientSoftware = str; }

void LocalTransport::start() { d->start(); }

void LocalTransport::setStunDiscoverer(AbstractStunDisco *discoverer) { d->setStunDiscoverer(discoverer); }

void LocalTransport::stop() { d->stop(); }

TransportAddress LocalTransport::localAddress() const
{
    return d->sock ? d->sock->localTransportAddress() : TransportAddress();
}

void LocalTransport::addChannelPeer(const TransportAddress &addr)
{
    if (d->turnClients)
        d->turnClients->addChannelPeer(addr);
}

bool LocalTransport::hasPendingDatagrams(int path) const
{
    if (path == Direct)
        return !d->in.isEmpty();
    else if (path == Relayed)
        return !d->inRelayed.isEmpty();
    else {
        Q_ASSERT(0);
        return false;
    }
}

QByteArray LocalTransport::readDatagram(int path, TransportAddress &addr)
{
    QList<Private::Datagram> *in = nullptr;
    if (path == Direct)
        in = &d->in;
    else if (path == Relayed)
        in = &d->inRelayed;
    else
        Q_ASSERT(0);

    if (!in->isEmpty()) {
        Private::Datagram datagram = in->takeFirst();
        addr                       = datagram.addr;
        return datagram.buf;
    } else
        return QByteArray();
}

void LocalTransport::writeDatagram(int path, const QByteArray &buf, const TransportAddress &addr)
{
    if (path == Direct) {
        Private::WriteItem wi;
        wi.type = Private::WriteItem::Direct;
        wi.addr = addr;
        d->pendingWrites += wi;
        d->sock->writeDatagram(buf, addr);
    } else if (path == Relayed) {
        if (d->turnClients && d->turnClients->isActivated())
            d->turnClients->write(buf, addr);
    } else
        Q_ASSERT(0);
}

void LocalTransport::setDebugLevel(DebugLevel level)
{
    d->debugLevel = level;
    if (d->pool)
        d->pool->setDebugLevel((StunTransactionPool::DebugLevel)level);
    if (d->turnClients)
        d->turnClients->setDebugLevel((TurnClient::DebugLevel)level);
}

void LocalTransport::changeThread(QThread *thread)
{
    if (d->pool)
        d->pool->moveToThread(thread);
    moveToThread(thread);
}

} // namespace XMPP

#include "icelocaltransport.moc"
