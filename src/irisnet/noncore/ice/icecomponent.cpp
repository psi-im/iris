/*
 * Copyright (C) 2010  Barracuda Networks, Inc.
 * Copyright (C) 2013-2021 Psi IM Team
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

#include "icecomponent.h"

#include "iceagent.h"
#include "icelocaltransport.h"
#include "iceturntransport.h"
#include "objectsession.h"
#include "udpportreserver.h"

#include <QTimer>
#include <QUdpSocket>
#include <QUuid>
#include <QtCrypto>
#include <stdlib.h>

namespace XMPP::ICE {
static int calc_priority(int typePref, int localPref, int componentId)
{
    Q_ASSERT(typePref >= 0 && typePref <= 126);
    Q_ASSERT(localPref >= 0 && localPref <= 65535);
    Q_ASSERT(componentId >= 1 && componentId <= 256);

    int priority = (1 << 24) * typePref;
    priority += (1 << 8) * localPref;
    priority += (256 - componentId);
    return priority;
}

class Component::Private : public QObject {
    Q_OBJECT

public:
    class Config {
    public:
        QList<ICE::LocalAddress> localAddrs;

        // for example manually provided external address mapped to every local
        QList<Ice176::ExternalAddress> extAddrs;
    };

    /*class LocalTransport {
    public:
        QUdpSocket *             qsock;
        QHostAddress             addr;
        ICE::LocalTransport::Ptr sock;
        int                      network = -1; // network interface index
        bool                     isVpn   = false;
        bool                     started = false;
        QHostAddress             extAddr;
        bool                     ext_finished = false;
        bool                     borrowed     = false;
    };*/

    Component                      *q;
    ObjectSession                   sess;
    int                             id;
    QString                         clientSoftware;
    TurnClient::Proxy               proxy;
    UdpPortReserver                *portReserver = nullptr;
    Config                          pendingConfig;
    Config                          config;
    bool                            stopping = false;
    QList<ICE::LocalTransport::Ptr> udpTransports; // transport for local host-only candidates

    QList<Candidate> localCandidates;
    bool             useLocal      = true; // use local host candidates
    bool             localFinished = false;
    // bool                               stunFinished      = false;
    bool               gatheringComplete = false;
    int                debugLevel        = DL_Packet;
    AbstractStunDisco *stunDiscoverer    = nullptr;

    Private(Component *_q) : QObject(_q), q(_q), sess(this) { }

    ~Private() { qDeleteAll(udpTransports); }

    ICE::LocalTransport::Ptr createLocalTransport(QUdpSocket *socket, bool borrowedSocket, const ICE::LocalAddress &la)
    {
        auto lt = ICE::LocalTransport::make();
        lt->setSocket(socket, borrowedSocket, la);
        lt->setClientSoftwareNameAndVersion(clientSoftware);
        lt->setDebugLevel(Transport::DebugLevel(debugLevel));
        lt->setStunDiscoverer(stunDiscoverer);

        connect(lt.get(), &ICE::LocalTransport::started, this, &Private::lt_started);
        connect(lt.get(), &ICE::LocalTransport::stopped, this, [this, lt]() {
            if (eraseLocalTransport(lt))
                tryStopped();
        });
        connect(lt.get(), &ICE::LocalTransport::candidateFound, this,
                [lt, this](CandidateInfo::Ptr info) { handleNewCandidate(lt, info); });
        connect(lt.get(), &ICE::LocalTransport::error, this, [this, lt]([[maybe_unused]] int error) {
            if (eraseLocalTransport(lt))
                tryGatheringComplete();
        });
        connect(lt.get(), &ICE::LocalTransport::debugLine, this, &Private::lt_debugLine);
        return lt;
    }

    void update(QList<QUdpSocket *> *socketList)
    {
        Q_ASSERT(!stopping);
        // for now, only allow setting localAddrs once
        if (!pendingConfig.localAddrs.isEmpty() && config.localAddrs.isEmpty()) {
            for (const auto &la : std::as_const(pendingConfig.localAddrs)) {
                // skip duplicate addrs
                if (findLocalAddr(la.addr) != -1)
                    continue;

                QUdpSocket *qsock = nullptr;
                if (useLocal && socketList) {
                    qsock = takeFromSocketList(socketList, la.addr, this);
                }
                bool borrowedSocket = qsock != nullptr;
                if (!qsock) {
                    // otherwise, bind to random
                    qsock = new QUdpSocket(this);
                    if (!qsock->bind(la.addr, 0)) {
                        delete qsock;
                        emit q->debugLine("Warning: unable to bind to random port.");
                        continue;
                    }
                }

                config.localAddrs += la;
                auto lt = createLocalTransport(qsock, borrowedSocket, la);
                // lt->borrowed = borrowedSocket;
                udpTransports += lt;

                int port = qsock->localPort();
                lt->start();
                emit q->debugLine(QString("starting transport ") + la.addr.toString() + ';' + QString::number(port)
                                  + " for component " + QString::number(id));
            }
        }

        // extAddrs created on demand if present, but only once
        if (!pendingConfig.extAddrs.isEmpty() && config.extAddrs.isEmpty()) {
            config.extAddrs = pendingConfig.extAddrs;

            bool need_doExt = false;

            for (auto lt : std::as_const(udpTransports)) {
                // already assigned an ext address?  skip
                if (!lt->externalAddress().isNull())
                    continue;

                auto laddr = lt->localAddress();
                if (laddr.addr.protocol() == QAbstractSocket::IPv6Protocol)
                    continue;

                // find external address by address of local socket (external has to be configured that way)
                auto eaIt = std::find_if(config.extAddrs.constBegin(), config.extAddrs.constEnd(), [&](auto const &ea) {
                    return ea.base.addr == laddr.addr && (ea.portBase == -1 || ea.portBase == laddr.port);
                });

                if (eaIt != config.extAddrs.constEnd()) {
                    lt->extAddr = eaIt->addr;
                    if (lt->started)
                        need_doExt = true;
                }
            }

            if (need_doExt)
                QTimer::singleShot(0, this, [this]() {
                    if (stopping)
                        return;

                    ObjectSessionWatcher watch(&sess);

                    for (auto lt : std::as_const(udpTransports)) {
                        if (lt->started) {
                            int addrAt = findLocalAddr(lt->addr);
                            Q_ASSERT(addrAt != -1);

                            ensureExt(lt, addrAt); // will emit candidateAdded if everything goes well
                            if (!watch.isValid())
                                return;
                        }
                    }
                });
        }

        if (udpTransports.isEmpty() && !localFinished) {
            localFinished = true;
            sess.defer(q, "localFinished");
        }
        sess.defer(this, "tryGatheringComplete");
    }

    void stop()
    {
        Q_ASSERT(!stopping);

        stopping = true;

        // nothing to stop?
        if (allStopped()) {
            sess.defer(this, "postStop");
            return;
        }

        for (LocalTransport *lt : std::as_const(udpTransports))
            lt->sock->stop();
    }

    int peerReflexivePriority(std::shared_ptr<Transport> iceTransport, int path) const
    {
        int                        addrAt = -1;
        const ICE::LocalTransport *lt     = qobject_cast<const IceLocalTransport *>(iceTransport.data());
        if (lt) {
            auto it = std::find_if(udpTransports.begin(), udpTransports.end(),
                                   [&](auto const &a) { return a->sock == lt; });
            Q_ASSERT(it != udpTransports.end());
            addrAt = int(std::distance(udpTransports.begin(), it));
            if (path == 1) {
                // lower priority, but not as far as IceTurnTransport
                addrAt += 512;
            }
        } else if (auto tmp = iceTransport.dynamicCast<IceTurnTransport>()) {
            // lower priority by making it seem like the last nic
            if (tcpTurn.contains(tmp))
                addrAt = 1024;
        }

        return choose_default_priority(PeerReflexiveType, 65535 - addrAt, QNetworkInterface::Ethernet,
                                       id); // ethernet? really?
    }

    void flagPathAsLowOverhead(int id, const TransportAddress &addr)
    {
        int at = -1;
        for (int n = 0; n < localCandidates.count(); ++n) {
            if (localCandidates[n].id == id) {
                at = n;
                break;
            }
        }

        Q_ASSERT(at != -1);

        if (at == -1)
            return;

        Candidate &c = localCandidates[at];

        QSet<TransportAddress> &addrs = channelPeers[c.id];
        if (!addrs.contains(addr)) {
            addrs += addr;
            c.iceTransport->addChannelPeer(addr);
        }
    }

    void addLocalPeerReflexiveCandidate(const TransportAddress &addr, CandidateInfo::Ptr base, quint32 priority)
    {
        auto ci  = CandidateInfo::make();
        ci->addr = addr;
        ci->addr.addr.setScopeId(QString());
        ci->related     = base->addr;
        ci->base        = base->addr;
        ci->type        = PeerReflexiveType;
        ci->priority    = priority;
        ci->foundation  = Agent::instance()->foundation(PeerReflexiveType, ci->base.addr);
        ci->componentId = base->componentId;
        ci->network     = base->network;

        auto baseCand = std::find_if(localCandidates.begin(), localCandidates.end(), [&](auto const &c) {
            return c.info->base == base->base && c.info->type == HostType;
        });
        Q_ASSERT(baseCand != localCandidates.end());

        Candidate c;
        c.id           = getId();
        c.info         = ci;
        c.iceTransport = baseCand->iceTransport;
        c.path         = 0;

        localCandidates += c;

        emit q->candidateAdded(c);
    }

private:
    // localPref is the priority of the network interface being used for
    //   this candidate.  the value must be between 0-65535 and different
    //   interfaces must have different values.  if there is only one
    //   interface, the value should be 65535.
    static int choose_default_priority(CandidateType type, int localPref, QNetworkInterface::InterfaceType ifType,
                                       int componentId)
    {
        int typePref;
        if (type == HostType) {
            if (ifType == QNetworkInterface::Virtual) // vpn?
                typePref = 0;
            else
                typePref = 126;
        } else if (type == PeerReflexiveType)
            typePref = 110;
        else if (type == ServerReflexiveType)
            typePref = 100;
        else // RelayedType
            typePref = 0;

        return calc_priority(typePref, localPref, componentId);
    }

    static QUdpSocket *takeFromSocketList(QList<QUdpSocket *> *socketList, const QHostAddress &addr,
                                          QObject *parent = nullptr)
    {
        for (int n = 0; n < socketList->count(); ++n) {
            if ((*socketList)[n]->localAddress() == addr) {
                QUdpSocket *sock = socketList->takeAt(n);
                sock->setParent(parent);
                return sock;
            }
        }

        return nullptr;
    }

    int getId() const
    {
        for (int n = 0;; ++n) {
            bool found = false;
            for (const Candidate &c : localCandidates) {
                if (c.id == n) {
                    found = true;
                    break;
                }
            }

            if (!found)
                return n;
        }
    }

    int findLocalAddr(const QHostAddress &addr)
    {
        for (int n = 0; n < config.localAddrs.count(); ++n) {
            if (config.localAddrs[n].addr == addr)
                return n;
        }

        return -1;
    }

    void ensureExt(LocalTransport *lt, int addrAt)
    {
        if (!lt->extAddr.isNull() && !lt->ext_finished) {
            auto ci         = CandidateInfo::make();
            ci->addr.addr   = lt->extAddr;
            ci->addr.port   = lt->sock->localAddress().port;
            ci->type        = ServerReflexiveType;
            ci->componentId = id;
            ci->priority    = choose_default_priority(ci->type, 65535 - addrAt, lt->isVpn, ci->componentId);
            ci->base        = lt->sock->localAddress();
            ci->related     = ci->base;
            ci->network     = lt->network;
            ci->foundation  = Agent::instance()->foundation(ServerReflexiveType, ci->base.addr);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = lt->sock;
            c.path         = 0;

            lt->ext_finished = true;

            storeLocalNotReduntantCandidate(c);
        }
    }

    void handleNewCandidate(ICE::LocalTransport::Ptr lt, CandidateInfo::Ptr info)
    {
        int addrAt = findLocalAddr(lt->localAddress().addr);
        Q_ASSERT(addrAt != -1);

        if (info->type == RelayedType) {
            // lower priority by making it seem like the last nic
            addrAt += 1024;
        }
        info->id       = id;
        info->priority = choose_default_priority(info->type, 65535 - addrAt, lt->networkType(), id);

        ObjectSessionWatcher watch(&sess);

        Candidate c;
        c.id           = getId();
        c.info         = info;
        c.iceTransport = lt;

        storeLocalNotReduntantCandidate(c);

        if (!watch.isValid())
            return;

        tryGatheringComplete();
#if 0
        if (lt->sock->serverReflexiveAddress().isValid() && !lt->stun_finished) {
            // automatically assign ext to related leaps, if possible
            for (LocalTransport *i : std::as_const(udpTransports)) {
                if (i->extAddr.isNull() && i->sock->localAddress() == lt->sock->localAddress()) {
                    i->extAddr = lt->sock->serverReflexiveAddress().addr;
                    if (i->started) {
                        ensureExt(i, addrAt);
                        if (!watch.isValid())
                            return;
                    }
                }
            }

            auto ci         = CandidateInfo::make();
            ci->addr        = lt->serverReflexiveAddress();
            ci->base        = lt->localAddress();
            ci->related     = ci->base;
            ci->type        = ServerReflexiveType;
            ci->componentId = id;
            ci->priority    = choose_default_priority(ci->type, 65535 - addrAt, lt->isVpn, ci->componentId);
            ci->network     = lt->network;
            ci->foundation  = Agent::instance()->foundation(
                ServerReflexiveType, ci->base.addr, lt->sock->reflexiveAddressSource(), QAbstractSocket::UdpSocket);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = sock->sharedFromThis();
            c.path         = 0;

            storeLocalNotReduntantCandidate(c);
        }

        if (lt->sock->relayedAddress().isValid() && !lt->turn_finished) {
            auto ci         = CandidateInfo::make();
            ci->addr        = lt->sock->relayedAddress();
            ci->base        = ci->addr;
            ci->related     = lt->sock->serverReflexiveAddress();
            ci->type        = RelayedType;
            ci->componentId = id;
            ci->priority    = choose_default_priority(ci->type, 65535 - addrAt, lt->isVpn, ci->componentId);
            ci->network     = lt->network;
            ci->foundation  = Agent::instance()->foundation(
                RelayedType, ci->base.addr, lt->sock->stunRelayServiceAddress().addr, QAbstractSocket::UdpSocket);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = sock->sharedFromThis();
            c.path         = 1;

            storeLocalNotReduntantCandidate(c);
        }
        if (!watch.isValid())
            return;

        tryGatheringComplete();
#endif
    }

    void storeLocalNotReduntantCandidate(const Candidate &c)
    {
        ObjectSessionWatcher watch(&sess);
        // RFC8445 5.1.3.  Eliminating Redundant Candidates
        auto it = std::find_if(localCandidates.begin(), localCandidates.end(), [&](const Candidate &cc) {
            return cc.info->addr == c.info->addr && cc.info->base == c.info->base
                && cc.info->priority >= c.info->priority;
        });
        if (it == localCandidates.end()) { // not reduntant
            localCandidates += c;
            emit q->candidateAdded(c);
        }
    }

    bool allStopped() const { return udpTransports.isEmpty(); }

    void tryStopped()
    {
        if (allStopped())
            postStop();
    }

    // return true if component is still alive after transport removal
    bool eraseLocalTransport(ICE::LocalTransport::Ptr lt)
    {
        ObjectSessionWatcher watch(&sess);

        emit q->debugLine(QLatin1String("Stopping local transport: ") + lt->localAddress().addr.toString());
        if (!watch.isValid())
            return false;

        lt->disconnect(this);
        auto sock = lt->takeBorrowedSocket();
        if (sock) {
            sock->disconnect(this);
            portReserver->returnSockets({ sock });
        }
        udpTransports.removeOne(lt);
        return true;
    }

private slots:
    void tryGatheringComplete()
    {
        if (gatheringComplete || (stunDiscoverer && stunDiscoverer->isDiscoInProgress()))
            return;

        if (std::any_of(tcpTurn.begin(), tcpTurn.end(), [](auto const &t) { return t->isStarted(); }))
            return;

        auto checkFinished = [&](const LocalTransport *lt) {
            return lt->started && (!lt->sock->stunBindServiceAddress().isValid() || lt->stun_finished)
                && (!lt->sock->stunRelayServiceAddress().isValid() || lt->turn_finished);
        };

        bool allFinished = true;
        for (const LocalTransport *lt : std::as_const(udpTransports)) {
            if (!checkFinished(lt)) {
                allFinished = false;
                break;
            }
        }

        if (allFinished) {
            gatheringComplete = true;
            emit q->gatheringComplete();
        }
    }

    void postStop()
    {
        stopping = false;

        emit q->stopped();
    }

    void lt_started()
    {
        IceLocalTransport *sock = static_cast<IceLocalTransport *>(sender());

        auto it
            = std::find_if(udpTransports.begin(), udpTransports.end(), [&](auto const &a) { return a->sock == sock; });
        Q_ASSERT(it != udpTransports.end());
        LocalTransport *lt = *it;

        lt->started = true;

        int addrAt = findLocalAddr(lt->addr);
        Q_ASSERT(addrAt != -1);

        ObjectSessionWatcher watch(&sess);

        if (useLocal) {
            auto ci         = CandidateInfo::make();
            ci->addr        = lt->sock->localAddress();
            ci->type        = HostType;
            ci->componentId = id;
            ci->priority    = choose_default_priority(ci->type, 65535 - addrAt, lt->isVpn, ci->componentId);
            ci->base        = ci->addr;
            ci->network     = lt->network;
            ci->foundation  = Agent::instance()->foundation(HostType, ci->base.addr);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = sock->sharedFromThis();
            c.path         = 0;

            localCandidates += c;

            emit q->candidateAdded(c);
            if (!watch.isValid())
                return;

            ensureExt(lt, addrAt);
            if (!watch.isValid())
                return;
        }

        if (!lt->stun_started) {
            lt->stun_started = true;
            if (!useStunBind || !expectMoreStunServers)
                lt->stun_finished = true;
        }

        // check completeness of various stuff
        if (!localFinished) {
            bool allStarted = true;
            for (const LocalTransport *lt : std::as_const(udpTransports)) {
                if (!lt->started) {
                    allStarted = false;
                    break;
                }
            }
            if (allStarted) {
                localFinished = true;
                emit q->localFinished();
                if (!watch.isValid())
                    return;
            }
        }

        tryGatheringComplete();
    }

    void lt_debugLine(const QString &line) { emit q->debugLine(line); }
};

Component::Component(int id, QObject *parent) : QObject(parent)
{
    d     = new Private(this);
    d->id = id;
}

Component::~Component() { delete d; }

int Component::id() const { return d->id; }

bool Component::isGatheringComplete() const { return d->gatheringComplete; }

void Component::setClientSoftwareNameAndVersion(const QString &str) { d->clientSoftware = str; }

void Component::setProxy(const TurnClient::Proxy &proxy) { d->proxy = proxy; }

void Component::setPortReserver(UdpPortReserver *portReserver) { d->portReserver = portReserver; }

UdpPortReserver *Component::portReserver() const { return d->portReserver; }

void Component::setLocalAddresses(const QList<ICE::LocalAddress> &addrs) { d->pendingConfig.localAddrs = addrs; }

void Component::setExternalAddresses(const QList<Ice176::ExternalAddress> &addrs) { d->pendingConfig.extAddrs = addrs; }

void Component::setUseLocal(bool enabled) { d->useLocal = enabled; }

void Component::setStunDiscoverer(AbstractStunDisco *discoverer) { d->stunDiscoverer = discoverer; }

void Component::update(QList<QUdpSocket *> *socketList) { d->update(socketList); }

void Component::stop() { d->stop(); }

int Component::peerReflexivePriority(std::shared_ptr<Transport> iceTransport, int path) const
{
    return d->peerReflexivePriority(iceTransport, path);
}

void Component::addLocalPeerReflexiveCandidate(const TransportAddress &addr, CandidateInfo::Ptr base, quint32 priority)
{
    d->addLocalPeerReflexiveCandidate(addr, base, priority);
}

void Component::flagPathAsLowOverhead(int id, const TransportAddress &addr)
{
    return d->flagPathAsLowOverhead(id, addr);
}

void Component::setDebugLevel(DebugLevel level)
{
    d->debugLevel = level;
    for (const auto &lt : std::as_const(d->udpTransports))
        lt->setDebugLevel(Transport::DebugLevel(level));
}

} // namespace XMPP

#include "icecomponent.moc"
