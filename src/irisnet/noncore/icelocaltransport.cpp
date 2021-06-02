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

#include "objectsession.h"
#include "stunallocate.h"
#include "stunbinding.h"
#include "stunmessage.h"
#include "stuntransaction.h"
#include "turnclient.h"

#include <QHostAddress>
#include <QUdpSocket>
#include <QtCrypto>

// don't queue more incoming packets than this per transmit path
#define MAX_PACKET_QUEUE 64

namespace XMPP {
enum { Direct, Relayed };

//----------------------------------------------------------------------------
// SafeUdpSocket
//----------------------------------------------------------------------------
// DOR-safe wrapper for QUdpSocket
class SafeUdpSocket : public QObject {
    Q_OBJECT

private:
    ObjectSession sess;
    QUdpSocket *  sock;
    int           writtenCount;

public:
    SafeUdpSocket(QUdpSocket *_sock, QObject *parent = nullptr) : QObject(parent), sess(this), sock(_sock)
    {
        sock->setParent(this);
        connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
        connect(sock, SIGNAL(bytesWritten(qint64)), SLOT(sock_bytesWritten(qint64)));

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

    QHostAddress localAddress() const { return sock->localAddress(); }

    quint16 localPort() const { return sock->localPort(); }

    bool hasPendingDatagrams() const { return sock->hasPendingDatagrams(); }

    QByteArray readDatagram(QHostAddress *address = nullptr, quint16 *port = nullptr)
    {
        if (!sock->hasPendingDatagrams())
            return QByteArray();

        QByteArray buf;
        buf.resize(int(sock->pendingDatagramSize()));
        sock->readDatagram(buf.data(), buf.size(), address, port);
        return buf;
    }

    void writeDatagram(const QByteArray &buf, const QHostAddress &address, quint16 port)
    {
        sock->writeDatagram(buf, address, port);
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
// IceLocalTransport
//----------------------------------------------------------------------------
class IceLocalTransport::Private : public QObject {
    Q_OBJECT

public:
    class WriteItem {
    public:
        enum Type { Direct, Pool, Turn };

        Type         type;
        QHostAddress addr;
        int          port;
    };

    class Written {
    public:
        QHostAddress addr;
        int          port;
        int          count;
    };

    class Datagram {
    public:
        QHostAddress addr;
        quint16      port;
        QByteArray   buf;
    };

    IceLocalTransport *      q;
    ObjectSession            sess;
    QUdpSocket *             extSock;
    SafeUdpSocket *          sock;
    StunTransactionPool::Ptr pool;
    StunBinding *            stunBinding;
    TurnClient *             turn;
    bool                     turnActivated;
    QHostAddress             addr;
    int                      port;
    QHostAddress             refAddr;
    int                      refPort;
    QHostAddress             refAddrSource;
    QHostAddress             relAddr;
    int                      relPort;
    QHostAddress             stunBindAddr;
    int                      stunBindPort;
    QHostAddress             stunRelayAddr;
    int                      stunRelayPort;
    QString                  stunUser;
    QCA::SecureArray         stunPass;
    QString                  clientSoftware;
    QList<Datagram>          in;
    QList<Datagram>          inRelayed;
    QList<WriteItem>         pendingWrites;
    int                      retryCount;
    bool                     stopping;
    int                      debugLevel;

    Private(IceLocalTransport *_q) :
        QObject(_q), q(_q), sess(this), extSock(nullptr), sock(nullptr), pool(nullptr), stunBinding(nullptr),
        turn(nullptr), turnActivated(false), port(-1), refPort(-1), relPort(-1), retryCount(0), stopping(false),
        debugLevel(IceTransport::DL_None)
    {
    }

    ~Private() { reset(); }

    void reset()
    {
        sess.reset();

        delete stunBinding;
        stunBinding = nullptr;

        delete turn;
        turn          = nullptr;
        turnActivated = false;

        if (sock) { // if started
            if (extSock) {
                sock->release(); // detaches the socket but doesn't destroy
                extSock = nullptr;
            }

            delete sock;
            sock = nullptr;
        }

        addr = QHostAddress();
        port = -1;

        refAddr       = QHostAddress();
        refPort       = -1;
        refAddrSource = QHostAddress();

        relAddr = QHostAddress();
        relPort = -1;

        in.clear();
        inRelayed.clear();
        pendingWrites.clear();

        retryCount = 0;
        stopping   = false;
    }

    void start()
    {
        Q_ASSERT(!sock);

        sess.defer(this, "postStart");
    }

    void stop()
    {
        Q_ASSERT(sock);
        if (stopping) {
            auto as = QString("%1:%2").arg(addr.toString(), QString::number(port));
            emit q->debugLine(QString("local transport %1 is already stopping. just wait...").arg(as));
            return;
        } else {
            auto as = QString("%1:%2").arg(addr.toString(), QString::number(port));
            emit q->debugLine(QString("stopping local transport %1.").arg(as));
        }

        stopping = true;

        if (turn)
            turn->close(); // will emit stopped() eventually calling postStop()
        else
            sess.defer(this, "postStop");
    }

    void stunStart()
    {
        Q_ASSERT(!pool);

        pool = StunTransactionPool::Ptr::create(StunTransaction::Udp);
        pool->setDebugLevel((StunTransactionPool::DebugLevel)debugLevel);
        connect(pool.data(), SIGNAL(outgoingMessage(QByteArray, QHostAddress, quint16)),
                SLOT(pool_outgoingMessage(QByteArray, QHostAddress, quint16)));
        connect(pool.data(), SIGNAL(needAuthParams()), SLOT(pool_needAuthParams()));
        connect(pool.data(), SIGNAL(debugLine(QString)), SLOT(pool_debugLine(QString)));

        pool->setLongTermAuthEnabled(true);
        if (!stunUser.isEmpty()) {
            pool->setUsername(stunUser);
            pool->setPassword(stunPass);
        }

        do_stun();
        do_turn();
    }

    void do_stun()
    {
        if (stunBindAddr.isNull()) {
            return;
        }
        stunBinding = new StunBinding(pool.data());
        connect(stunBinding, &StunBinding::success, this, [&]() {
            refAddr       = stunBinding->reflexiveAddress();
            refPort       = stunBinding->reflexivePort();
            refAddrSource = stunBindAddr;

            delete stunBinding;
            stunBinding = nullptr;

            emit q->addressesChanged();
        });
        connect(stunBinding, &StunBinding::error, this, [&](XMPP::StunBinding::Error) {
            delete stunBinding;
            stunBinding = nullptr;
            emit q->error(IceLocalTransport::ErrorStun);
        });
        stunBinding->start(stunBindAddr, stunBindPort);
    }

    void do_turn()
    {
        if (stunRelayAddr.isNull()) {
            return;
        }
        turn = new TurnClient(this);
        turn->setDebugLevel((TurnClient::DebugLevel)debugLevel);
        connect(turn, &TurnClient::connected, this, &Private::turn_connected);
        connect(turn, &TurnClient::tlsHandshaken, this, &Private::turn_tlsHandshaken);
        connect(turn, &TurnClient::closed, this, &Private::turn_closed);
        connect(turn, &TurnClient::activated, this, &Private::turn_activated);
        connect(turn, &TurnClient::packetsWritten, this, &Private::turn_packetsWritten);
        connect(turn, &TurnClient::error, this, &Private::turn_error);
        connect(turn, &TurnClient::outgoingDatagram, this, &Private::turn_outgoingDatagram);
        connect(turn, &TurnClient::debugLine, this, &Private::turn_debugLine);

        turn->setClientSoftwareNameAndVersion(clientSoftware);

        turn->connectToHost(pool.data(), stunRelayAddr, stunRelayPort);
    }

private:
    // note: emits signal on error
    QUdpSocket *createSocket()
    {
        QUdpSocket *qsock = new QUdpSocket(this);
        if (!qsock->bind(addr, 0)) {
            delete qsock;
            emit q->error(IceLocalTransport::ErrorBind);
            return nullptr;
        }

        return qsock;
    }

    void prepareSocket()
    {
        addr = sock->localAddress();
        port = sock->localPort();

        connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
        connect(sock, SIGNAL(datagramsWritten(int)), SLOT(sock_datagramsWritten(int)));
    }

    // return true if we are retrying, false if we should error out
    bool handleRetry()
    {
        // don't allow retrying if activated or stopping)
        if (turnActivated || stopping)
            return false;

        ++retryCount;
        if (retryCount < 3) {
            if (debugLevel >= IceTransport::DL_Info)
                emit q->debugLine("retrying...");

            delete sock;
            sock = nullptr;

            // to receive this error, it is a Relay, so change
            //   the mode
            // stunType = IceLocalTransport::Relay;

            QUdpSocket *qsock = createSocket();
            if (!qsock) {
                // signal emitted in this case.  bail.
                //   (return true so caller takes no action)
                return true;
            }

            sock = new SafeUdpSocket(qsock, this);

            prepareSocket();

            refAddr       = QHostAddress();
            refPort       = -1;
            refAddrSource = QHostAddress();

            relAddr = QHostAddress();
            relPort = -1;

            do_turn();

            // tell the world that our local address probably
            //   changed, and that we lost our reflexive address
            emit q->addressesChanged();
            return true;
        }

        return false;
    }

    // return true if data packet, false if pool or nothing
    bool processIncomingStun(const QByteArray &buf, const QHostAddress &fromAddr, int fromPort, Datagram *dg)
    {
        QByteArray   data;
        QHostAddress dataAddr;
        int          dataPort;

        bool notStun;
        if (!pool->writeIncomingMessage(buf, &notStun, fromAddr, fromPort) && turn) {
            data = turn->processIncomingDatagram(buf, notStun, &dataAddr, &dataPort);
            if (!data.isNull()) {
                dg->addr = dataAddr;
                dg->port = dataPort;
                dg->buf  = data;
                return true;
            } else {
                if (debugLevel >= IceTransport::DL_Packet)
                    emit q->debugLine(
                        "Warning: server responded with what doesn't seem to be a STUN or data packet, skipping.");
            }
        }

        return false;
    }

private slots:
    void postStart()
    {
        if (stopping)
            return;

        if (extSock) {
            sock = new SafeUdpSocket(extSock, this);
        } else {
            QUdpSocket *qsock = createSocket();
            if (!qsock) {
                // signal emitted in this case.  bail
                return;
            }

            sock = new SafeUdpSocket(qsock, this);
        }

        prepareSocket();

        emit q->started();
    }

    void postStop()
    {
        reset();
        emit q->stopped();
    }

    void sock_readyRead()
    {
        ObjectSessionWatcher watch(&sess);

        QList<Datagram> dreads; // direct
        QList<Datagram> rreads; // relayed

        while (sock->hasPendingDatagrams()) {
            QHostAddress from;
            quint16      fromPort;

            Datagram dg;

            QByteArray buf = sock->readDatagram(&from, &fromPort);
            if (buf.isEmpty()) // it's weird we ever came here, but should relax static analyzer
                break;
            if ((from == stunBindAddr && fromPort == stunBindPort)
                || (from == stunRelayAddr && fromPort == stunRelayPort)) {
                bool haveData = processIncomingStun(buf, from, fromPort, &dg);

                // processIncomingStun could cause signals to
                //   emit.  for example, stopped()
                if (!watch.isValid())
                    return;

                if (haveData)
                    rreads += dg;
            } else {
                dg.addr = from;
                dg.port = fromPort;
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
                    if (dwrites[n].addr == wi.addr && dwrites[n].port == wi.port) {
                        at = n;
                        break;
                    }
                }

                if (at != -1) {
                    ++dwrites[at].count;
                } else {
                    Written wr;
                    wr.addr  = wi.addr;
                    wr.port  = wi.port;
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
            for (const Written &wr : dwrites) {
                emit q->datagramsWritten(Direct, wr.count, wr.addr, wr.port);
                if (!watch.isValid())
                    return;
            }
        }

        if (twrites > 0) {
            // note: this will invoke turn_packetsWritten()
            turn->outgoingDatagramsWritten(twrites);
        }
    }

    void pool_outgoingMessage(const QByteArray &packet, const QHostAddress &toAddress, quint16 toPort)
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

        sock->writeDatagram(packet, toAddress, toPort);
    }

    void pool_needAuthParams()
    {
        // we can get this signal if the user did not provide
        //   creds to us.  however, since this class doesn't support
        //   prompting just continue on as if we had a blank
        //   user/pass
        pool->continueAfterParams();
    }

    void pool_debugLine(const QString &line) { emit q->debugLine(line); }

    void turn_connected()
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine("turn_connected");
    }

    void turn_tlsHandshaken()
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine("turn_tlsHandshaken");
    }

    void turn_closed()
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine("turn_closed");

        delete turn;
        turn          = nullptr;
        turnActivated = false;

        postStop();
    }

    void turn_activated()
    {
        StunAllocate *allocate = turn->stunAllocate();

        // take reflexive address from TURN only if we are not using a
        //   separate STUN server
        if (stunBindAddr.isNull() || stunBindAddr == stunRelayAddr) {
            refAddr       = allocate->reflexiveAddress();
            refPort       = allocate->reflexivePort();
            refAddrSource = stunRelayAddr;
        }

        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine(QString("Server says we are ") + allocate->reflexiveAddress().toString() + ';'
                              + QString::number(allocate->reflexivePort()));

        relAddr = allocate->relayedAddress();
        relPort = allocate->relayedPort();
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine(QString("Server relays via ") + relAddr.toString() + ';' + QString::number(relPort));

        turnActivated = true;

        emit q->addressesChanged();
    }

    void turn_packetsWritten(int count, const QHostAddress &addr, int port)
    {
        emit q->datagramsWritten(Relayed, count, addr, port);
    }

    void turn_error(XMPP::TurnClient::Error e)
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine(QString("turn_error: ") + turn->errorString());

        delete turn;
        turn              = nullptr;
        bool wasActivated = turnActivated;
        turnActivated     = false;

        if (e == TurnClient::ErrorMismatch) {
            if (!extSock && handleRetry())
                return;
        }

        // this means our relay died on us.  in the future we might
        //   consider reporting this
        if (wasActivated)
            return;

        emit q->error(IceLocalTransport::ErrorTurn);
        // don't report any error
        // if(stunType == IceLocalTransport::Relay || (stunType == IceLocalTransport::Auto && !stunBinding))
        //    emit q->addressesChanged();
    }

    void turn_outgoingDatagram(const QByteArray &buf)
    {
        WriteItem wi;
        wi.type = WriteItem::Turn;
        pendingWrites += wi;
        sock->writeDatagram(buf, stunRelayAddr, quint16(stunRelayPort));
    }

    void turn_debugLine(const QString &line) { emit q->debugLine(line); }
};

IceLocalTransport::IceLocalTransport(QObject *parent) : IceTransport(parent) { d = new Private(this); }

IceLocalTransport::~IceLocalTransport() { delete d; }

void IceLocalTransport::setClientSoftwareNameAndVersion(const QString &str) { d->clientSoftware = str; }

void IceLocalTransport::start(QUdpSocket *sock)
{
    d->extSock = sock;
    d->start();
}

void IceLocalTransport::start(const QHostAddress &addr)
{
    d->addr = addr;
    d->start();
}

void IceLocalTransport::stop() { d->stop(); }

void IceLocalTransport::setStunBindService(const QHostAddress &addr, int port)
{
    d->stunBindAddr = addr;
    d->stunBindPort = port;
}

void IceLocalTransport::setStunRelayService(const QHostAddress &addr, int port, const QString &user,
                                            const QCA::SecureArray &pass)
{
    d->stunRelayAddr = addr;
    d->stunRelayPort = port;
    d->stunUser      = user;
    d->stunPass      = pass;
}

QHostAddress IceLocalTransport::stunBindServiceAddress() const { return d->stunBindAddr; }

QHostAddress IceLocalTransport::stunRelayServiceAddress() const { return d->stunRelayAddr; }

void IceLocalTransport::stunStart() { d->stunStart(); }

QHostAddress IceLocalTransport::localAddress() const { return d->addr; }

int IceLocalTransport::localPort() const { return d->port; }

QHostAddress IceLocalTransport::serverReflexiveAddress() const { return d->refAddr; }

int IceLocalTransport::serverReflexivePort() const { return d->refPort; }

QHostAddress IceLocalTransport::reflexiveAddressSource() const { return d->refAddrSource; }

QHostAddress IceLocalTransport::relayedAddress() const { return d->relAddr; }

int IceLocalTransport::relayedPort() const { return d->relPort; }

bool IceLocalTransport::isStunAlive() const { return d->stunBinding != nullptr; }

bool IceLocalTransport::isTurnAlive() const { return d->turn != nullptr; }

void IceLocalTransport::addChannelPeer(const QHostAddress &addr, int port)
{
    if (d->turn)
        d->turn->addChannelPeer(addr, port);
}

bool IceLocalTransport::hasPendingDatagrams(int path) const
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

QByteArray IceLocalTransport::readDatagram(int path, QHostAddress *addr, quint16 *port)
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
        *addr                      = datagram.addr;
        *port                      = datagram.port;
        return datagram.buf;
    } else
        return QByteArray();
}

void IceLocalTransport::writeDatagram(int path, const QByteArray &buf, const QHostAddress &addr, int port)
{
    if (path == Direct) {
        Private::WriteItem wi;
        wi.type = Private::WriteItem::Direct;
        wi.addr = addr;
        wi.port = port;
        d->pendingWrites += wi;
        d->sock->writeDatagram(buf, addr, quint16(port));
    } else if (path == Relayed) {
        if (d->turn && d->turnActivated)
            d->turn->write(buf, addr, port);
    } else
        Q_ASSERT(0);
}

void IceLocalTransport::setDebugLevel(DebugLevel level)
{
    d->debugLevel = level;
    if (d->pool)
        d->pool->setDebugLevel((StunTransactionPool::DebugLevel)level);
    if (d->turn)
        d->turn->setDebugLevel((TurnClient::DebugLevel)level);
}

void IceLocalTransport::changeThread(QThread *thread)
{
    if (d->pool)
        d->pool->moveToThread(thread);
    moveToThread(thread);
}

} // namespace XMPP

#include "icelocaltransport.moc"
