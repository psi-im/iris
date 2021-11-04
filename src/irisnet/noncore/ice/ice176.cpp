/*
 * Copyright (C) 2009-2010  Barracuda Networks, Inc.
 * Copyright (C) 2020  Sergey Ilinykh
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

#include "ice176.h"

#include "iceabstractstundisco.h"
#include "iceagent.h"
#include "icecandidate.h"
#include "icecomponent.h"
#include "icelocaltransport.h"
#include "iceturntransport.h"
#include "iputil.h"
#include "localaddress.h"
#include "stun/stunbinding.h"
#include "stun/stunmessage.h"
#include "stun/stuntransaction.h"
#include "stun/stuntypes.h"
#include "stun/turnclient.h"
#include "udpportreserver.h"

#include <QDeadlineTimer>
#include <QEvent>
#include <QNetworkInterface>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include <QtCrypto>

#define ICE_DEBUG
#ifdef ICE_DEBUG
#define iceDebug qDebug
#else
#define iceDebug(...)
#endif

namespace XMPP {
enum { Direct, Relayed };

static qint64 calc_pair_priority(int a, int b)
{
    qint64 priority = ((qint64)1 << 32) * qMin(a, b);
    priority += (qint64)2 * qMax(a, b);
    if (a > b)
        ++priority;
    return priority;
}

class Ice176::Private : public QObject {
    Q_OBJECT

public:
    // note, Nominating state is skipped when aggressive nomination is enabled.
    enum State {
        Stopped,
        Starting, // preparing local candidates right after start() call
        Started,  // local candidates ready. ready for pairing with remote
        Active,   // all components have a nominated pair and media transferred over them
        Stopping  // when received a command from the user to stop
    };

    enum CandidatePairState { PWaiting, PInProgress, PSucceeded, PFailed, PFrozen };

    enum CheckListState { LRunning, LCompleted, LFailed };

    class CandidatePair {
    public:
        using Ptr = std::shared_ptr<CandidatePair>;

        ICE::CandidateInfo::Ptr local, remote;
        bool                    isDefault   = false; // not used in xmpp
        bool                    isValid     = false; // a pair which is also in valid list
        bool                    isNominated = false; // nomination is confirmed by the peer.

        // states for last or comming checks
        bool isTriggered             = false; // last scheduled check was a triggered check
        bool isTriggeredForNominated = false;
        bool finalNomination         = false;
#ifdef ICE_DEBUG
        bool logNew = false;
#endif

        CandidatePairState state = CandidatePairState::PFrozen;

        qint64  priority = 0;
        QString foundation; // rfc8445 6.1.2.6 (combination of foundations)

        StunBinding *binding = nullptr;

        // FIXME: this is wrong i think, it should be in LocalTransport
        //   or such, to multiplex ids
        StunTransactionPool::Ptr pool;

        inline bool isNull() const { return local->addr.addr.isNull() || remote->addr.addr.isNull(); }
        inline      operator QString() const
        {
            if (isNull())
                return QLatin1String("null pair");
            return QString(QLatin1String("L:%1 %2 - R:%3 %4 (prio:%5)"))
                .arg(candidateType_to_string(local->type), QString(local->addr), candidateType_to_string(remote->type),
                     QString(remote->addr), QString::number(priority));
        }
    };

    class CheckList {
    public:
        QList<std::shared_ptr<CandidatePair>> pairs;
        QQueue<std::weak_ptr<CandidatePair>>  triggeredPairs;
        QList<std::shared_ptr<CandidatePair>> validPairs; // highest priority and nominated come first
        CheckListState                        state;
    };

    class Component {
    public:
        int                     id              = 0;
        ICE::Component         *ic              = nullptr;
        std::unique_ptr<QTimer> nominationTimer = std::unique_ptr<QTimer>();
        CandidatePair::Ptr      selectedPair; // final selected pair. won't be changed
        CandidatePair::Ptr      highestPair;  // current highest priority pair to send data
        bool                    localFinished     = false;
        bool                    hasValidPairs     = false;
        bool                    hasNominatedPairs = false;
        bool                    stopped           = false;
        bool                    lowOverhead       = false;

        // initiator is nominating the final pair (will be set as `selectePair` when ready)
        bool nominating = false; // with aggressive nomination it's always false
    };

    Ice176                             *q;
    Ice176::Mode                        mode;
    State                               state = Stopped;
    QTimer                              checkTimer;
    TurnClient::Proxy                   proxy;
    UdpPortReserver                    *portReserver = nullptr;
    std::unique_ptr<QTimer>             pacTimer;
    int                                 nominationTimeout = 3000; // 3s
    int                                 pacTimeout     = 30000; // 30s todo: compute from rto. see draft-ietf-ice-pac-06
    int                                 componentCount = 1;
    QList<ICE::LocalAddress>            localAddrs;
    QList<Ice176::ExternalAddress>      extAddrs;
    QPointer<AbstractStunDisco>         stunDiscoverer;
    QString                             localUser, localPass;
    QString                             peerUser, peerPass;
    std::vector<Component>              components;
    QList<ICE::Component::Candidate>    localCandidates;
    QList<ICE::CandidateInfo::Ptr>      remoteCandidates;
    QSet<std::weak_ptr<ICE::Transport>> iceTransports;
    CheckList                           checkList;
    QList<QList<QByteArray>>            in;
    Features                            remoteFeatures;
    Features                            localFeatures;
    bool                                allowIpExposure            = true;
    bool                                useLocal                   = true;
    bool                                localHostGatheringFinished = false;
    bool                                localGatheringComplete     = false;
    bool                                remoteGatheringComplete    = false;
    bool                                readyToSendMedia           = false;
    bool                                canStartChecks             = false;

    Private(Ice176 *_q) : QObject(_q), q(_q)
    {
        connect(&checkTimer, &QTimer::timeout, this, [this]() {
            auto pair = selectNextPairToCheck();
            if (pair)
                checkPair(pair);
            else
                checkTimer.stop();
        });
        checkTimer.setInterval(20);
        checkTimer.setSingleShot(false);
    }

    ~Private()
    {
        for (const Component &c : components)
            delete c.ic;
    }

    void reset() { checkTimer.stop(); /*TODO*/ }

    int findLocalAddress(const QHostAddress &addr)
    {
        for (int n = 0; n < localAddrs.count(); ++n) {
            if (localAddrs[n].addr == addr)
                return n;
        }

        return -1;
    }

    void updateLocalAddresses(const QList<ICE::LocalAddress> &addrs)
    {
        // for now, ignore address changes during operation
        if (state != Stopped)
            return;

        localAddrs.clear();
        for (const auto &la : addrs) {
            int at = findLocalAddress(la.addr);
            if (at == -1)
                localAddrs += la;
        }
    }

    void updateExternalAddresses(const QList<ExternalAddress> &addrs)
    {
        // for now, ignore address changes during operation
        if (state != Stopped)
            return;

        extAddrs.clear();
        for (const ExternalAddress &ea : addrs) {
            int at = findLocalAddress(ea.base.addr);
            if (at != -1)
                extAddrs += ea;
        }
    }

    void start()
    {
        Q_ASSERT(state == Stopped);
        Q_ASSERT(componentCount > 0 && componentCount < (65536 - 1024));

        state = Starting;

        localUser = ICE::Agent::randomCredential(4);
        localPass = ICE::Agent::randomCredential(22);

        QList<QUdpSocket *> socketList;
        if (portReserver)
            // list size = componentCount * number of interfaces
            socketList = portReserver->borrowSockets(componentCount, this);

        components.reserve(ulong(componentCount));
        for (int n = 0; n < componentCount; ++n) {
            components.emplace_back();
            Component &c = components.back();
            c.id         = n + 1;
            c.ic         = new ICE::Component(c.id, this);
            c.ic->setDebugLevel(ICE::Component::DL_Packet);
            connect(c.ic, &ICE::Component::candidateAdded, this, &Private::ic_candidateAdded);
            connect(c.ic, &ICE::Component::localFinished, this, &Private::ic_localFinished);
            connect(c.ic, &ICE::Component::gatheringComplete, this, &Private::ic_gatheringComplete);
            connect(c.ic, &ICE::Component::stopped, this, &Private::ic_stopped);
            connect(c.ic, &ICE::Component::debugLine, this, &Private::ic_debugLine);

            c.ic->setClientSoftwareNameAndVersion("Iris");
            c.ic->setProxy(proxy);
            if (portReserver)
                c.ic->setPortReserver(portReserver);
            c.ic->setLocalAddresses(localAddrs);
            c.ic->setExternalAddresses(extAddrs);
            c.ic->setUseLocal(useLocal && allowIpExposure);
            c.ic->setStunDiscoverer(stunDiscoverer);

            // create an inbound queue for this component
            in += QList<QByteArray>();

            c.ic->update(&socketList);
        }

        // socketList should always empty here, but might not be if
        //   the app provided a different address list to
        //   UdpPortReserver and Ice176.  and that would really be
        //   a dumb thing to do but I'm not going to Q_ASSERT it
        if (!socketList.isEmpty())
            portReserver->returnSockets(socketList);
    }

    void startChecks()
    {
        pacTimer.reset(new QTimer(this));
        pacTimer->setSingleShot(true);
        pacTimer->setInterval(pacTimeout);
        connect(pacTimer.get(), &QTimer::timeout, this, &Ice176::Private::onPacTimeout);
        iceDebug("Start Patiently Awaiting Connectivity timer");
        canStartChecks = true;
        pacTimer->start();
        checkTimer.start();
    }

    void stop()
    {
        if (state == Stopped || state == Stopping)
            return; // stopped as a result of previous error?

        canStartChecks = false;
        state          = Stopping;
        pacTimer.reset();
        checkTimer.stop();

        // will trigger candidateRemoved events and result pairs cleanup.
        if (!components.empty()) {
            for (auto &c : components) {
                c.nominationTimer.reset();
                c.ic->stop();
            }

        } else {
            QMetaObject::invokeMethod(this, "postStop", Qt::QueuedConnection);
        }
    }

    void addRemoteCandidates(const QList<Candidate> &list)
    {
        QList<ICE::CandidateInfo::Ptr> remoteCandidates;
        for (const Candidate &c : list) {
            auto ci       = ICE::CandidateInfo::make();
            ci->addr.addr = c.ip;
            ci->addr.addr.setScopeId(QString());
            ci->addr.port   = c.port;
            ci->type        = (ICE::CandidateType)string_to_candidateType(c.type); // TODO: handle error
            ci->componentId = c.component;
            ci->priority    = c.priority;
            ci->foundation  = c.foundation;
            if (!c.rel_addr.isNull()) {
                ci->base.addr = c.rel_addr;
                ci->base.addr.setScopeId(QString());
                ci->base.port = c.rel_port;
            }
            ci->network = c.network;
            ci->id      = c.id;

            // find remote prflx with same addr. we have to update them instead adding new one. RFC8445 7.3.1.3
            auto it = std::find_if(this->remoteCandidates.begin(), this->remoteCandidates.end(),
                                   [&](ICE::CandidateInfo::Ptr rc) {
                                       return ci->addr == rc->addr && ci->componentId == rc->componentId
                                           && rc->type == ICE::PeerReflexiveType;
                                   });
            if (it != this->remoteCandidates.end()) {
                (*it)->type = ci->type; // RFC8445 5.1.2.1.  Recommended Formula (peer-reflexive are preferred)
                                        // B.7.  Why Prefer Peer-Reflexive Candidates?
                                        // if srflx == prflx -> set srflx because not secure anyway
                (*it)->foundation = ci->foundation;
                (*it)->base       = ci->base;
                (*it)->network    = ci->network;
                (*it)->id         = ci->id;
                iceDebug("Previously known remote prflx was updated from signalling: %s", qPrintable((*it)->addr));
            } else {
                remoteCandidates += ci;
            }
        }
        this->remoteCandidates += remoteCandidates;

        iceDebug("adding %d remote candidates. total=%d", remoteCandidates.count(), this->remoteCandidates.count());
        doPairing(localCandidates, remoteCandidates);
    }

    void setRemoteGatheringComplete()
    {
        remoteGatheringComplete = true;
        if (!localGatheringComplete || state != Started)
            return;

        for (auto &c : components)
            tryNominateSelectedPair(c.id);
    }

    // returns a pair is pairable or null
    std::shared_ptr<CandidatePair> makeCandidatesPair(ICE::CandidateInfo::Ptr lc, ICE::CandidateInfo::Ptr rc)
    {
        if (lc->componentId != rc->componentId)
            return {};

        // don't pair ipv4 with ipv6.  FIXME: is this right?
        if (lc->addr.addr.protocol() != rc->addr.addr.protocol()) {
            iceDebug("Skip building pair: %s - %s (protocol mismatch)", qPrintable(lc->addr), qPrintable(rc->addr));
            return {};
        }

        // don't relay to localhost.  turnserver
        //   doesn't like it.  i don't know if this
        //   should qualify as a HACK or not.
        //   trying to relay to localhost is pretty
        //   stupid anyway
        if (lc->type == ICE::RelayedType && IpUtil::isLoopbackAddress(rc->addr.addr)) {
            qDebug("Skip building pair: %s - %s (relay to localhost)", qPrintable(lc->addr), qPrintable(rc->addr));
            return {};
        }

        auto pair    = std::make_shared<CandidatePair>();
        pair->local  = lc;
        pair->remote = rc;
        if (pair->local->addr.addr.protocol() == QAbstractSocket::IPv6Protocol
            && IpUtil::isLinkLocalAddress(pair->local->addr.addr))
            pair->remote->addr.addr.setScopeId(pair->local->addr.addr.scopeId());
        if (mode == Ice176::Initiator)
            pair->priority = calc_pair_priority(lc->priority, rc->priority);
        else
            pair->priority = calc_pair_priority(rc->priority, lc->priority);

        return pair;
    }

    // adds new pairs, sorts, prunes
    void addChecklistPairs(const QList<std::shared_ptr<CandidatePair>> &pairs)
    {
#ifdef ICE_DEBUG
        iceDebug("%d new pairs", pairs.count());
        for (auto &p : pairs)
            p->logNew = true;
#endif
        if (!pairs.count())
            return;

        // combine pairs with existing, and sort
        checkList.pairs += pairs;
        std::sort(checkList.pairs.begin(), checkList.pairs.end(),
                  [&](const std::shared_ptr<CandidatePair> &a, const std::shared_ptr<CandidatePair> &b) {
                      return a->priority == b->priority ? a->local->componentId < b->local->componentId
                                                        : a->priority > b->priority;
                  });

        // pruning
        for (int n = 0; n < checkList.pairs.count(); ++n) {
            auto &pair = checkList.pairs[n];
#ifdef ICE_DEBUG
            if (pair->logNew)
                iceDebug("C%d, %s", pair->local->componentId, qPrintable(*pair));
#endif

            for (int i = n - 1; i >= 0; --i) {
                // RFC8445 says to use base only for reflexive. but base is set properly for host and relayed too.
                if (pair->local->componentId == checkList.pairs[i]->local->componentId
                    && pair->local->base == checkList.pairs[i]->local->base
                    && pair->remote->addr == checkList.pairs[i]->remote->addr) {

                    checkList.pairs.removeAt(n);
                    --n; // adjust position
                    break;
                }
            }
        }

        // max pairs is 100 * number of components
        int max_pairs = 100 * int(components.size());
        while (checkList.pairs.count() > max_pairs)
            checkList.pairs.removeLast();
#ifdef ICE_DEBUG
        iceDebug("%d after pruning (just new below):", checkList.pairs.count());
        for (auto &p : checkList.pairs) {
            if (p->logNew)
                iceDebug("C%d, %s", p->local->componentId, qPrintable(*p));
            p->logNew = false;
        }
#endif
    }

    std::shared_ptr<CandidatePair> selectNextPairToCheck()
    {
        // rfc8445 6.1.4.2.  Performing Connectivity Checks
        std::shared_ptr<CandidatePair> pair;
        while (!checkList.triggeredPairs.empty() && !(pair = checkList.triggeredPairs.dequeue().lock()))
            ;

        if (pair) {
            pair->isTriggered = true;
            // according to rfc - check just this one
            iceDebug("next check from triggered list: %s", qPrintable(*pair));
            return pair;
        }

        auto it = std::find_if(checkList.pairs.begin(), checkList.pairs.end(), [&](const auto &p) mutable {
            if (p->state == PFrozen && !pair)
                pair = p;
            return p->state == PWaiting;
        });
        if (it != checkList.pairs.end()) { // found waiting
            // the list was sorted already by priority and componentId. So first one is Ok
            iceDebug("next check for already waiting: %s", qPrintable(**it));
            (*it)->isTriggered = false;
            return *it;
        }

        if (pair) { // now it's frozen highest-priority pair
            pair->isTriggered = false;
            iceDebug("next check for a frozen pair: %s", qPrintable(*pair));
        }

        // FIXME real algo should be (but is requires significant refactoring)
        //   1) go over all knows pair foundations over all checklists
        //   2) if for the foundation there is a frozen pair but no (in-progress or waiting)
        //   3)    - do checks on this pair

        return pair;
    }

    void checkPair(std::shared_ptr<CandidatePair> pair)
    {
        pair->foundation = pair->local->foundation + pair->remote->foundation;
        pair->state      = PInProgress;

        int at = findLocalCandidate(pair->local->addr);
        Q_ASSERT(at != -1);

        auto &lc = localCandidates[at];

        Component &c = *findComponent(lc.info->componentId);

        // read comment to the pool member how wrong it is
        pair->pool = StunTransactionPool::Ptr::create(StunTransaction::Udp);
        connect(pair->pool.data(), &StunTransactionPool::outgoingMessage, this,
                [this, weakPair = decltype(pair)::weak_type(pair)](const QByteArray &packet, const TransportAddress &) {
                    auto pair = weakPair.lock();
                    if (!pair)
                        return;
                    int at = findLocalCandidate(pair->local->addr);
                    if (at == -1) { // FIXME: assert?
                        qDebug("Failed to find local candidate %s", qPrintable(pair->local->addr));
                        return;
                    }

                    ICE::Component::Candidate &lc          = localCandidates[at];
                    auto                       stunSession = lc.stunSession.lock();
                    if (!stunSession) {
                        // TODO
                    }

                    iceDebug("send connectivity check for pair %s%s", qPrintable(*pair),
                             (mode == Initiator
                                  ? (pair->binding->useCandidate() ? " (nominating)" : "")
                                  : (pair->isTriggeredForNominated ? " (triggered check for nominated)" : "")));
                    lc.iceTransport->writeDatagram(path, packet, pair->remote->addr);
                });

        // pair->pool->setUsername(peerUser + ':' + localUser);
        // pair->pool->setPassword(peerPass.toUtf8());

        pair->binding = new StunBinding(pair->pool.data());
        connect(pair->binding, &StunBinding::success, this, [this, wpair = decltype(pair)::weak_type(pair)]() {
            auto pair = wpair.lock();
            if (pair)
                handlePairBindingSuccess(pair);
        });
        connect(pair->binding, &StunBinding::error, this,
                [this, wpair = decltype(pair)::weak_type(pair)](XMPP::StunBinding::Error e) {
                    auto pair = wpair.lock();
                    if (pair)
                        handlePairBindingError(pair, e);
                });

        quint32 prflx_priority = c.ic->peerReflexivePriority(lc.iceTransport, lc.path);
        pair->binding->setPriority(prflx_priority);

        if (mode == Ice176::Initiator) {
            pair->binding->setIceControlling(0);
            if (localFeatures & AggressiveNomination || pair->finalNomination)
                pair->binding->setUseCandidate(true);
        } else
            pair->binding->setIceControlled(0);

        pair->binding->setShortTermUsername(peerUser + ':' + localUser);
        pair->binding->setShortTermPassword(peerPass);

        pair->binding->start();
    }

    void doPairing(const QList<ICE::Component::Candidate> &localCandidates,
                   const QList<ICE::CandidateInfo::Ptr>   &remoteCandidates)
    {
        QList<std::shared_ptr<CandidatePair>> pairs;
        for (const ICE::Component::Candidate &cc : localCandidates) {
            auto lc = cc.info;
            if (lc->type == ICE::PeerReflexiveType) {
                iceDebug("not pairing local prflx. %s", qPrintable(lc->addr));
                // see RFC8445 7.2.5.3.1.  Discovering Peer-Reflexive Candidates
                continue;
            }

            for (const ICE::CandidateInfo::Ptr &rc : qAsConst(remoteCandidates)) {
                auto pair = makeCandidatesPair(lc, rc);
                if (pair)
                    pairs += pair;
            }
        }

        if (!pairs.count())
            return;

        addChecklistPairs(pairs);
        if (canStartChecks && !checkTimer.isActive())
            checkTimer.start();
    }

    void write(int componentIndex, const QByteArray &datagram)
    {
        auto cIt = findComponent(componentIndex + 1);
        Q_ASSERT(cIt != components.end());

        auto pair = cIt->selectedPair;
        if (!pair) {
            pair = cIt->highestPair;
            if (!pair) {
                iceDebug("An attempt to write to an ICE component w/o valid sockets");
                return;
            }
        }

        int at = findLocalCandidate(pair->local->addr);
        if (at == -1) { // FIXME: assert?
            iceDebug("FIXME! Failed to find local candidate for componentId=%d, addr=%s", componentIndex + 1,
                     qPrintable(pair->local->addr));
            return;
        }

        ICE::Component::Candidate &lc = localCandidates[at];

        int path = lc.path;

        lc.iceTransport->writeDatagram(path, datagram, pair->remote->addr);

        // DOR-SR?
        QMetaObject::invokeMethod(q, "datagramsWritten", Qt::QueuedConnection, Q_ARG(int, componentIndex),
                                  Q_ARG(int, 1));
    }

    void flagComponentAsLowOverhead(int componentIndex)
    {
        Q_ASSERT(size_t(componentIndex) < components.size());
        // FIXME: ok to assume in order?
        Component &c  = components[componentIndex];
        c.lowOverhead = true;

        // FIXME: actually do something
    }

    void cleanupButSelectedPair(int componentId)
    {
        CandidatePair::Ptr selected = findComponent(componentId)->selectedPair;
        Q_ASSERT(selected);
        decltype(checkList.validPairs) newValid;
        newValid.push_back(selected);
        for (auto &p : checkList.validPairs)
            if (p->local->componentId != componentId)
                newValid.push_back(p);
        checkList.validPairs = newValid;

        auto t = findTransport(selected->local->base);
        Q_ASSERT(t.get() != nullptr);

        // cancel planned/active transactions
        QMutableListIterator<std::weak_ptr<CandidatePair>> it(checkList.triggeredPairs);
        while (it.hasNext()) {
            auto p = it.next().lock();
            if (!p || p->local->componentId == componentId)
                it.remove();
        }
        for (auto &p : checkList.pairs) {
            if (p->local->componentId == componentId && p->state == PInProgress) {
                p->binding->cancel();
                p->state = PFailed;
                iceDebug("Cancel %s setting it to failed state", qPrintable(*p));
            }
        }
        // stop not used transports
        for (auto &c : localCandidates) {
            if (c.info->componentId == componentId && c.iceTransport != t) {
                c.iceTransport->stop();
            }
        }
    }

    void setSelectedPair(int componentId)
    {
        auto &component = *findComponent(componentId);
        auto &pair      = component.selectedPair;
        if (pair || (stunDiscoverer && stunDiscoverer->isDiscoInProgress()))
            return;
#ifdef ICE_DEBUG
        iceDebug("Current valid list state:");
        for (auto &p : checkList.validPairs) {
            iceDebug("  C%d: %s", p->local->componentId, qPrintable(*p));
        }
#endif
        component.nominationTimer.reset();
        pair = component.highestPair;
        if (!pair) {
            qWarning("C%d: failed to find selected pair for previously nominated component. Candidates removed "
                     "without ICE restart?",
                     componentId);
            stop();
            emit q->error(ErrorGeneric);
            return;
        }
        iceDebug("C%d: selected pair: %s (base: %s)", componentId, qPrintable(*pair), qPrintable(pair->local->base));
        cleanupButSelectedPair(componentId);
        emit q->componentReady(componentId - 1);
        tryIceFinished();
    }

    void optimizeCheckList(int componentId)
    {
        auto it = findComponent(componentId);
        Q_ASSERT(it != components.end() && it->highestPair);

        auto minPriority = it->highestPair->priority;
        for (auto &p : checkList.pairs) {
            bool toStop = p->local->componentId == componentId && (p->state == PFrozen || p->state == PWaiting)
                && p->priority < minPriority;
            if (toStop) {
                iceDebug("Disable check for %s since we already have better valid pairs", qPrintable(*p));
                p->state = PFailed;
            }
        }
        for (auto &pWeak : checkList.triggeredPairs) {
            auto p = pWeak.lock();
            if (p->local->componentId == componentId && p->priority < minPriority) {
                iceDebug("Disable triggered check for %s since we already have better valid pairs", qPrintable(*p));
                p->state = PFailed;
            }
        }
    }

    bool doesItWorthNominateNow(int componentId)
    {
        auto &c = *findComponent(componentId);
        if (mode != Initiator || (localFeatures & AggressiveNomination) || state != Started || !c.highestPair
            || c.selectedPair || c.nominating)
            return false;

        auto pair = c.highestPair;
        Q_ASSERT(!pair->isNominated);
        if (pair->local->type == ICE::RelayedType) {
            if (!(localGatheringComplete && remoteGatheringComplete)) {
                iceDebug("Waiting for gathering complete on both sides before nomination of relayed pair");
                return false; // maybe we gonna have a non-relayed pair. RFC8445 anyway allows to send data on any
                              // valid.
            }

            // if there is any non-relayed pending pair
            if (std::any_of(checkList.pairs.begin(), checkList.pairs.end(), [](auto &p) {
                    return p->state != PSucceeded && p->state != PFailed && p->local->type != ICE::RelayedType;
                })) {
                iceDebug("There are some non-relayed pairs to check before relayed nomination");
                return false; // either till checked or remote gathering timeout
            }
        }
        return true;
    }

    void nominateSelectedPair(int componentId)
    {
        auto &c = *findComponent(componentId);
        Q_ASSERT(mode == Initiator && c.highestPair && !c.selectedPair && !c.nominating);
        c.nominationTimer.reset();
        c.nominating                   = true;
        c.highestPair->finalNomination = true;
        iceDebug("Nominating valid pair: %s", qPrintable(*c.highestPair));
        checkList.triggeredPairs.prepend(c.highestPair);
        if (!checkTimer.isActive())
            checkTimer.start();
    }

    void tryNominateSelectedPair(int componentId)
    {
        if (doesItWorthNominateNow(componentId))
            nominateSelectedPair(componentId);
    }

    void tryIceFinished()
    {
        if (!std::all_of(components.begin(), components.end(), [](auto &c) { return c.selectedPair != nullptr; }))
            return;
        tryReadyToSendMedia(); // just send it before finished in case it was missed
#ifdef ICE_DEBUG
        iceDebug("ICE selected final pairs!");
        for (auto &c : components) {
            iceDebug("  C%d: %s", c.id, qPrintable(*c.selectedPair));
        }
        iceDebug("Signalling iceFinished now");
#endif
        pacTimer.reset();
        state = Active;
        emit q->iceFinished();
    }

    /**
     * For aggressive nomination this method setups a timer to select final pair for the component
     * So we call this function after we already have a nominated pair already and want to finished with the component.
     *
     * For non-aggressive it setups a timer to nominate highest priority valid pair.
     * For Responder in non-aggressive mode it does nothing.
     * @param componentId
     */
    void setupNominationTimer(int componentId)
    {
        if (!stunDiscoverer || stunDiscoverer->isDiscoInProgress())
            return;
        Component &c = *findComponent(componentId);
        if (c.nominationTimer)
            return;
        bool useAggressiveNom = bool((mode == Initiator ? localFeatures : remoteFeatures) & AggressiveNomination);
        if (!useAggressiveNom && mode == Responder)
            return; // responder will wait for nominated pairs till very end

        auto timer = new QTimer();
        c.nominationTimer.reset(timer);
        timer->setSingleShot(true);
        timer->setInterval(nominationTimeout);
        connect(timer, &QTimer::timeout, this, [this, componentId, useAggressiveNom]() {
            Q_ASSERT(state == Started);
            Component &c = *findComponent(componentId);
            c.nominationTimer.release()->deleteLater();
            if (c.stopped)
                return; // already queue signal likely
            if (useAggressiveNom)
                setSelectedPair(componentId);
            else if (!c.nominating && !c.selectedPair)
                nominateSelectedPair(componentId); // let the peer know about selected pair. the final step for the comp
        });
        timer->start();
    }

    // nominated - out side=responder. and remote request had USE_CANDIDATE
    void doTriggeredCheck(const ICE::Component::Candidate &locCand, ICE::CandidateInfo::Ptr remCand,
                          bool triggeredForPreviouslyNominated)
    {
        // let's figure out if this pair already in the check list
        auto it = std::find_if(checkList.pairs.begin(), checkList.pairs.end(),
                               [&](auto const &p) { return *(p->local) == locCand.info && *(p->remote) == remCand; });

        CandidatePair::Ptr pair        = (it == checkList.pairs.end()) ? CandidatePair::Ptr() : *it;
        Component         &component   = *findComponent(locCand.info->componentId);
        qint64             minPriority = component.highestPair ? component.highestPair->priority : 0;
        if (pair) {
            if (pair->priority < minPriority) {
                iceDebug(
                    "Don't do triggered check for known pair since the pair has lower priority than highest valid");
                return;
            }
            if (pair->state == CandidatePairState::PSucceeded) {
                // Check nominated here?
                iceDebug("Don't do triggered check since pair is already in success state");
                if (mode == Responder && !pair->isNominated && triggeredForPreviouslyNominated) {
                    pair->isNominated = true;
                    onNewValidPair(pair); // responders do not nominate but we changed isNominated flag so let's process
                }
                return; // nothing todo. rfc 8445 7.3.1.4
            }
            pair->isNominated = false;
            if (pair->state == CandidatePairState::PInProgress) {
                if (pair->isTriggered) {
                    iceDebug(
                        "Current in-progress check is already triggered. Don't cancel it while have to according to "
                        "RFC8445\n");
                    return;
                }
                pair->binding->cancel();
            }
            if (pair->state == PFailed) {
                // if (state == Stopped) {
                // TODO Stopped? maybe Failed? and we have to notify the outer world
                //}
            }
        } else {
            // RFC8445 7.3.1.4.  Triggered Checks / "If the pair is not already on the checklist"
            pair = makeCandidatesPair(locCand.info, remCand);
            if (!pair) {
                return;
            }
            if (pair->priority < minPriority) {
                iceDebug(
                    "Don't do triggered check for a new pair since the pair has lower priority than highest valid");
                return;
            }
            addChecklistPairs(QList<CandidatePair::Ptr>() << pair);
        }

        pair->state                   = PWaiting;
        pair->isTriggeredForNominated = triggeredForPreviouslyNominated;
        checkList.triggeredPairs.enqueue(pair);

        if (canStartChecks && !checkTimer.isActive())
            checkTimer.start();
    }

    void onPacTimeout()
    {
        Q_ASSERT(state == Starting || state == Started);
        pacTimer.release()->deleteLater();
        iceDebug("Patiently Awaiting Connectivity timeout");
        stop();
        emit q->error(ErrorGeneric);
    }

private:
    inline decltype(components)::iterator findComponent(const ICE::Component *ic)
    {
        return std::find_if(components.begin(), components.end(), [&](auto &c) { return c.ic == ic; });
    }

    inline decltype(components)::iterator findComponent(int id)
    {
        return std::find_if(components.begin(), components.end(), [&](auto &c) { return c.id == id; });
    }

    int findLocalCandidate(const ICE::Transport *iceTransport, int path, bool hostAndRelayOnly = false) const
    {
        for (int n = 0; n < localCandidates.count(); ++n) {
            const ICE::Component::Candidate &cc = localCandidates[n];
            if (cc.iceTransport == iceTransport && cc.path == path
                && (!hostAndRelayOnly || cc.info->type == ICE::RelayedType || cc.info->type == ICE::HostType))
                return n;
        }

        return -1;
    }

    int findLocalCandidate(const TransportAddress &fromAddr)
    {
        for (int n = 0; n < localCandidates.count(); ++n) {
            if (localCandidates[n].info->addr == fromAddr)
                return n;
        }

        return -1;
    }

    std::shared_ptr<ICE::Transport> findTransport(const TransportAddress &addr)
    {
        auto c = findLocalCandidate(addr);
        if (c >= 0)
            return localCandidates[c].iceTransport;
        return {};
    }

    static QString candidateType_to_string(ICE::CandidateType type)
    {
        QString out;
        switch (type) {
        case ICE::HostType:
            out = "host";
            break;
        case ICE::PeerReflexiveType:
            out = "prflx";
            break;
        case ICE::ServerReflexiveType:
            out = "srflx";
            break;
        case ICE::RelayedType:
            out = "relay";
            break;
        default:
            Q_ASSERT(0);
        }
        return out;
    }

    static int string_to_candidateType(const QString &in)
    {
        if (in == "host")
            return ICE::HostType;
        else if (in == "prflx")
            return ICE::PeerReflexiveType;
        else if (in == "srflx")
            return ICE::ServerReflexiveType;
        else if (in == "relay")
            return ICE::RelayedType;
        else
            return -1;
    }

    static void toOutCandidate(const ICE::Component::Candidate &cc, Ice176::Candidate &out)
    {
        out.component  = cc.info->componentId;
        out.foundation = cc.info->foundation;
        out.generation = 0; // TODO
        out.id         = cc.info->id;
        out.ip         = cc.info->addr.addr;
        out.ip.setScopeId(QString());
        out.network  = cc.info->network;
        out.port     = cc.info->addr.port;
        out.priority = cc.info->priority;
        out.protocol = "udp";
        if (cc.info->type != ICE::HostType) {
            out.rel_addr = cc.info->related.addr;
            out.rel_addr.setScopeId(QString());
            out.rel_port = cc.info->related.port;
        } else {
            out.rel_addr = QHostAddress();
            out.rel_port = -1;
        }
        out.rem_addr = QHostAddress();
        out.rem_port = -1;
        out.type     = candidateType_to_string(cc.info->type);
    }

    void dumpCandidatesAndStart()
    {
        QList<Ice176::Candidate> list;
        for (auto const &cc : qAsConst(localCandidates)) {
            Ice176::Candidate c;
            toOutCandidate(cc, c);
            list += c;
        }
        if (list.size())
            emit q->localCandidatesReady(list);

        state = Started;
        emit q->started();
        if (mode == Responder)
            doPairing(localCandidates, remoteCandidates);
    }

    QString generateIdForCandidate()
    {
        QString id;
        do {
            id = ICE::Agent::randomCredential(10);
        } while (std::find_if(localCandidates.begin(), localCandidates.end(),
                              [&id](auto const &c) { return c.info->id == id; })
                 != localCandidates.end());
        return id;
    }

    void tryReadyToSendMedia()
    {
        if (readyToSendMedia) {
            return;
        }
        bool allowNotNominatedData = (localFeatures & NotNominatedData) && (remoteFeatures & NotNominatedData);
        // if both follow RFC8445 and allow to send data on any valid pair
        if (!std::all_of(components.begin(), components.end(),
                         [&](auto &c) { return (allowNotNominatedData && c.hasValidPairs) || c.hasNominatedPairs; })) {
            return;
        }
#ifdef ICE_DEBUG
        iceDebug("Ready to send media!");
        for (auto &c : components) {
            if (c.selectedPair)
                iceDebug("  C%d: selected pair: %s (base: %s)", c.id, qPrintable(*c.selectedPair),
                         qPrintable(c.selectedPair->local->base));
            else {
                iceDebug("  C%d: any pair from valid list", c.id);
                iceDebug("       highest: %s", qPrintable(*c.highestPair));
            }
        }
#endif
        readyToSendMedia = true;
        emit q->readyToSendMedia();
    }

    void insertIntoValidList(int componentId, CandidatePair::Ptr pair)
    {
        auto &component = *findComponent(componentId);
        if (!component.selectedPair) { // no final pair yet
            // find position to insert in sorted list of valid pairs
            auto insIt = std::upper_bound(
                checkList.validPairs.begin(), checkList.validPairs.end(), pair, [](auto &item, auto &toins) {
                    return item->priority == toins->priority
                        ? item->local->componentId < toins->local->componentId
                        : item->priority >= toins->priority; // inverted since we need high priority first
                });

            bool highest = false;
            if (!component.highestPair || component.highestPair->priority < pair->priority) {
                component.highestPair = pair;
                highest               = true;
            }
            checkList.validPairs.insert(insIt, pair); // nominated and highest priority first
            iceDebug("C%d: insert to valid list %s%s", component.id, qPrintable(*pair),
                     highest ? " (as highest priority)" : "");
        }
    }

    /**
     * Process a pair just marked as valid or/and nominated
     * @param pair
     */
    void onNewValidPair(CandidatePair::Ptr pair)
    {
        auto &component          = *findComponent(pair->local->componentId);
        bool  alreadyInValidList = pair->isValid;
        pair->isValid            = true;
        pair->state              = PSucceeded; // what if it was in progress?

        component.hasValidPairs = true;

        // mark all with same foundation as Waiting to prioritize them (see RFC8445 7.2.5.3.3)
        for (auto &p : checkList.pairs)
            if (p->state == PFrozen && p->foundation == pair->foundation)
                p->state = PWaiting;

        if (!alreadyInValidList)
            insertIntoValidList(component.id, pair);

        optimizeCheckList(component.id);

        // if (c.lowOverhead) { // commented out since we need turn permissions for all components
        iceDebug("component is flagged for low overhead.  setting up for %s", qPrintable(*pair));
        auto &cc = localCandidates[findLocalCandidate(pair->local->addr)];
        component.ic->flagPathAsLowOverhead(cc.id, pair->remote->addr);
        //}

        if (pair->isNominated) { // just nominated. either confirmed on initator or recognized as nominted on responder
            component.hasNominatedPairs = true;
            bool aggrNom = bool((mode == Initiator ? localFeatures : remoteFeatures) & AggressiveNomination);
            if (!aggrNom) {
                // w/o aggressive nomination only one pair can be nominated and we already have it. So let's select it.
                setSelectedPair(component.id);
            } else
                setupNominationTimer(component.id);
        } else // just marked valid
            setupNominationTimer(component.id);
        tryReadyToSendMedia();
    }

    void handlePairBindingSuccess(CandidatePair::Ptr pair)
    {
        /*
            RFC8445 7.2.5.2.1.  Non-Symmetric Transport Addresses
            tells us addr:port of source->dest of request MUST match with dest<-source of the response,
            and we should mark the pair as failed if doesn't match.
            But StunTransaction already does this for us in its checkActiveAndFrom.
            So it will fail with timeout instead if response comes from a wrong address.
        */

        StunBinding *binding = pair->binding;
        // pair->isValid = true;
        pair->state                   = CandidatePairState::PSucceeded;
        bool  isTriggeredForNominated = pair->isTriggeredForNominated;
        bool  isNominatedByInitiator  = mode == Initiator && binding->useCandidate();
        bool  finalNomination         = pair->finalNomination;
        auto &component               = *findComponent(pair->local->componentId);

        iceDebug("check success for %s", qPrintable(QString(*pair)));

        // RFC8445 7.2.5.3.1.  Discovering Peer-Reflexive Candidates
        auto mappedAddr = binding->reflexiveAddress();
        // skip "If the valid pair equals the pair that generated the check"
        if (pair->local->addr != binding->reflexiveAddress()) {
            // so mapped address doesn't match with local candidate sending binding request.
            // gotta find/create one
            auto locIt = std::find_if(localCandidates.begin(), localCandidates.end(), [&](const auto &c) {
                return (c.info->base == mappedAddr || c.info->addr == mappedAddr)
                    && c.info->componentId == component.id;
            });
            if (locIt == localCandidates.end()) {
                // RFC8445 7.2.5.3.1.  Discovering Peer-Reflexive Candidates
                // new peer-reflexive local candidate discovered
                component.ic->addLocalPeerReflexiveCandidate(mappedAddr, pair->local, binding->priority());
                // find just inserted prflx candidate
                locIt = std::find_if(localCandidates.begin(), localCandidates.end(),
                                     [&](const auto &c) { return c.info->addr == mappedAddr; });
                Q_ASSERT(locIt != localCandidates.end());
                // local candidate wasn't found, so it wasn't on the checklist  RFC8445 7.2.5.3.1.3
                // allow v4/v6 proto mismatch in case NAT does magic
                pair = makeCandidatesPair(locIt->info, pair->remote);
            } else {
                // local candidate found. If it's a part of a pair on checklist, we have to add this pair to valid list,
                // otherwise we have to create a new pair and add it to valid list
                auto it = std::find_if(checkList.pairs.cbegin(), checkList.pairs.cend(), [&](auto const &p) {
                    return p->local->base == locIt->info->base && p->remote->addr == pair->remote->addr
                        && p->local->componentId == locIt->info->componentId;
                });
                if (it == checkList.pairs.end()) {
                    // allow v4/v6 proto mismatch in case NAT does magic
                    pair = makeCandidatesPair(locIt->info, pair->remote);
                } else {
                    pair = *it;
                    iceDebug("mapped address belongs to another pair on checklist %s", qPrintable(QString(*pair)));
                }
            }
        }

        if (!pair) {
            qWarning("binding success but failed to build a pair with mapped address %s!", qPrintable(mappedAddr));
            return;
        }

        pair->isTriggeredForNominated = isTriggeredForNominated;
        pair->finalNomination         = finalNomination;

        pair->isNominated = isTriggeredForNominated || isNominatedByInitiator; // both responder and initiator
        onNewValidPair(pair);
    }

    void handlePairBindingError(CandidatePair::Ptr pair, XMPP::StunBinding::Error)
    {
        Q_ASSERT(state != Stopped);
        if (state == Stopping)
            return; // we don't care about late errors

        if (state == Active) {
            iceDebug("todo! binding error ignored in Active state");
            return; // TODO hadle keep-alive binding properly
        }

        iceDebug("check failed for %s", qPrintable(*pair));
        auto &c     = *findComponent(pair->local->componentId);
        pair->state = CandidatePairState::PFailed;
        if (pair->isValid) { // RFC8445 7.2.5.3.4.  Updating the Nominated Flag /  about failure
            checkList.validPairs.removeOne(pair);
            pair->isValid = false;
            if (c.highestPair == pair) {
                // the failed binding is nomination or triggered after receiving success on canceled binding
                c.highestPair.reset();
            }
        }

        if ((c.nominating && pair->finalNomination)
            || (!(remoteFeatures & AggressiveNomination) && pair->isTriggeredForNominated)) {

            if (pair->isTriggeredForNominated)
                qInfo("Failed to do triggered check for nominated selectedPair. set ICE status to failed");
            else
                qInfo("Failed to nominate selected pair. set ICE status to failed");
            stop();
            emit q->error(ErrorDisconnected);
            return;
        }

        // if not nominating but use-candidate then I'm initiator with aggressive nomination. It's Ok to fail.
        // if nominating but not use-candidate then I'm initiator and something not important failed
    }

private slots:
    void postStop()
    {
        state = Stopped;
        emit q->stopped();
    }

    void ic_candidateAdded(const XMPP::ICE::Component::Candidate &_cc)
    {
        ICE::Component::Candidate cc = _cc;

        cc.info->id = generateIdForCandidate();

        localCandidates += cc;

        iceDebug("C%d: candidate added: %s %s;%d", cc.info->componentId,
                 qPrintable(candidateType_to_string(cc.info->type)), qPrintable(cc.info->addr.addr.toString()),
                 cc.info->addr.port);

        if (!iceTransports.contains(cc.iceTransport)) {
            connect(cc.iceTransport.get(), &ICE::Transport::readyRead, this, &Private::it_readyRead);
            connect(cc.iceTransport.get(), &ICE::Transport::datagramsWritten, this, &Private::it_datagramsWritten);

            iceTransports += cc.iceTransport;
        }

        if (!localHostGatheringFinished)
            return; // all local IPs will be reported at once

        if (localFeatures & Trickle) {
            QList<Ice176::Candidate> list;

            Ice176::Candidate c;
            toOutCandidate(cc, c);
            list += c;

            emit q->localCandidatesReady(list);
        }
        if (state == Started) {
            doPairing(QList<ICE::Component::Candidate>() << cc, remoteCandidates);
        }
    }

    void ic_candidateRemoved(const XMPP::ICE::Component::Candidate &cc)
    {
        // TODO
        iceDebug("C%d: candidate removed: %s;%d", cc.info->componentId, qPrintable(cc.info->addr.addr.toString()),
                 cc.info->addr.port);

        QStringList idList;
        for (int n = 0; n < localCandidates.count(); ++n) {
            if (localCandidates[n].id == cc.id && localCandidates[n].info->componentId == cc.info->componentId) {
                // FIXME: this is rather ridiculous I think
                idList += localCandidates[n].info->id;

                localCandidates.removeAt(n);
                --n; // adjust position
            }
        }

        bool iceTransportInUse = false;
        for (const ICE::Component::Candidate &lc : qAsConst(localCandidates)) {
            if (lc.iceTransport == cc.iceTransport) {
                iceTransportInUse = true;
                break;
            }
        }
        if (!iceTransportInUse) {
            cc.iceTransport->disconnect(this);
            iceTransports.remove(cc.iceTransport);
        }

        for (int n = 0; n < checkList.pairs.count(); ++n) {
            if (idList.contains(checkList.pairs[n]->local->id)) {
                StunBinding *binding = checkList.pairs[n]->binding;
                auto         pool    = checkList.pairs[n]->pool;

                delete binding;

                if (pool) {
                    pool->disconnect(this);
                }
                checkList.pairs[n]->pool.reset();

                checkList.pairs.removeAt(n);
                --n; // adjust position
            }
        }
    }

    void ic_localFinished()
    {
        ICE::Component *ic = static_cast<ICE::Component *>(sender());
        auto            it = findComponent(ic);
        Q_ASSERT(it != components.end());
        Q_ASSERT(!it->localFinished);

        it->localFinished = true;

        for (const Component &c : components) {
            if (!c.localFinished) {
                return;
            }
        }

        localHostGatheringFinished = true;
        if (localFeatures & Trickle)
            dumpCandidatesAndStart();
    }

    void ic_gatheringComplete()
    {
        if (localGatheringComplete)
            return; // wtf? Why are we here then

        for (auto const &c : components) {
            if (!c.ic->isGatheringComplete()) {
                return;
            }
        }
        localGatheringComplete = true;

        if (localFeatures & Trickle) { // It was already started
            emit q->localGatheringComplete();
            return;
        }

        dumpCandidatesAndStart();
    }

    void ic_stopped()
    {
        ICE::Component *ic = static_cast<ICE::Component *>(sender());
        auto            it = findComponent(ic);
        Q_ASSERT(it != components.end());

        it->stopped = true;
        it->nominationTimer.reset();

        bool allStopped = true;
        for (const Component &c : components) {
            if (!c.stopped) {
                allStopped = false;
                break;
            }
        }

        if (allStopped)
            postStop();
    }

    void ic_debugLine(const QString &line)
    {
#ifdef ICE_DEBUG
        ICE::Component *ic = static_cast<ICE::Component *>(sender());
        auto            it = findComponent(ic);
        Q_ASSERT(it != components.end());

        // FIXME: components are always sorted?
        iceDebug("C%d: %s", it->id, qPrintable(line));
#else
        Q_UNUSED(line)
#endif
    }

    // path is either direct or relayed
    void it_readyRead(int path)
    {
        ICE::Transport *it = static_cast<ICE::Transport *>(sender());
        int             at = findLocalCandidate(it, path, true); // just host or relay
        Q_ASSERT(at != -1);

        ICE::Component::Candidate &locCand = localCandidates[at];

        ICE::Transport *sock = it;

        while (sock->hasPendingDatagrams(path)) {
            TransportAddress fromAddr;
            QByteArray       buf = sock->readDatagram(path, fromAddr);

            // iceDebug("port %d: received packet (%d bytes)", lt->sock->localPort(), buf.size());

            QString    requser = localUser + ':' + peerUser;
            QByteArray reqkey  = localPass.toUtf8();

            StunMessage::ConvertResult result;
            StunMessage                msg = StunMessage::fromBinary(buf, &result,
                                                                     StunMessage::MessageIntegrity | StunMessage::Fingerprint, reqkey);
            if (!msg.isNull() && (msg.mclass() == StunMessage::Request || msg.mclass() == StunMessage::Indication)) {
                iceDebug("received validated request or indication from %s", qPrintable(fromAddr));
                QString user = QString::fromUtf8(msg.attribute(StunTypes::USERNAME));
                if (requser != user) {
                    iceDebug("user [%s] is wrong.  it should be [%s].  skipping", qPrintable(user),
                             qPrintable(requser));
                    continue;
                }

                if (msg.method() != StunTypes::Binding) {
                    iceDebug("not a binding request.  skipping");
                    continue;
                }

                StunMessage response;
                response.setClass(StunMessage::SuccessResponse);
                response.setMethod(StunTypes::Binding);
                response.setId(msg.id());

                QList<StunMessage::Attribute> list;
                StunMessage::Attribute        attr;
                attr.type  = StunTypes::XOR_MAPPED_ADDRESS;
                attr.value = StunTypes::createXorPeerAddress(fromAddr, response.magic(), response.id());
                list += attr;

                response.setAttributes(list);

                QByteArray packet = response.toBinary(StunMessage::MessageIntegrity | StunMessage::Fingerprint, reqkey);
                sock->writeDatagram(path, packet, fromAddr);

                if (state != Started) // only in started state we do triggered checks
                    return;

                auto it = std::find_if(
                    remoteCandidates.begin(), remoteCandidates.end(), [&](ICE::CandidateInfo::Ptr remCand) {
                        return remCand->componentId == locCand.info->componentId && remCand->addr == fromAddr;
                    });
                bool nominated = false;
                if (mode == Responder)
                    nominated = msg.hasAttribute(StunTypes::USE_CANDIDATE);
                if (it == remoteCandidates.end()) {
                    // RFC8445 7.3.1.3.  Learning Peer-Reflexive Candidates
                    iceDebug("found NEW remote prflx! %s", qPrintable(fromAddr));
                    quint32 priority;
                    StunTypes::parsePriority(msg.attribute(StunTypes::PRIORITY), &priority);
                    auto remCand = ICE::CandidateInfo::makeRemotePrflx(locCand.info->componentId, fromAddr, priority);
                    remoteCandidates += remCand;
                    doTriggeredCheck(locCand, remCand, nominated);
                } else {
                    doTriggeredCheck(locCand, *it, nominated);
                }
            } else {
                QByteArray  reskey = peerPass.toUtf8();
                StunMessage msg    = StunMessage::fromBinary(
                       buf, &result, StunMessage::MessageIntegrity | StunMessage::Fingerprint, reskey);
                if (!msg.isNull()
                    && (msg.mclass() == StunMessage::SuccessResponse || msg.mclass() == StunMessage::ErrorResponse)) {
                    iceDebug("received validated response from %s to %s", qPrintable(fromAddr),
                             qPrintable(locCand.info->addr));

                    // FIXME: this is so gross and completely defeats the point of having pools
                    for (int n = 0; n < checkList.pairs.count(); ++n) {
                        CandidatePair &pair = *checkList.pairs[n];
                        if (pair.state == PInProgress && pair.local->addr.addr == locCand.info->addr.addr
                            && pair.local->addr.port == locCand.info->addr.port)
                            pair.pool->writeIncomingMessage(msg);
                    }
                } else {
                    // iceDebug("received some non-stun or invalid stun packet");

                    // FIXME: i don't know if this is good enough
                    if (StunMessage::isProbablyStun(buf)) {
                        iceDebug("unexpected stun packet (loopback?), skipping.");
                        continue;
                    }

                    int at = -1;
                    for (int n = 0; n < checkList.pairs.count(); ++n) {
                        CandidatePair &pair = *checkList.pairs[n];
                        if (pair.local->addr.addr == locCand.info->addr.addr
                            && pair.local->addr.port == locCand.info->addr.port) {
                            at = n;
                            break;
                        }
                    }
                    if (at == -1) {
                        iceDebug("the local transport does not seem to be associated with a candidate?!");
                        continue;
                    }

                    int componentIndex = checkList.pairs[at]->local->componentId - 1;
                    // iceDebug("packet is considered to be application data for component index %d", componentIndex);

                    // FIXME: this assumes components are ordered by id in our local arrays
                    in[componentIndex] += buf;
                    emit q->readyRead(componentIndex);
                }
            }
        }
    }

    void it_datagramsWritten(int path, int count, const TransportAddress &addr)
    {
        // TODO
        Q_UNUSED(path);
        Q_UNUSED(count);
        Q_UNUSED(addr);
    }
};

Ice176::Ice176(QObject *parent) : QObject(parent) { d = new Private(this); }

Ice176::~Ice176() { delete d; }

void Ice176::reset() { d->reset(); }

void Ice176::setProxy(const TurnClient::Proxy &proxy) { d->proxy = proxy; }

void Ice176::setPortReserver(UdpPortReserver *portReserver)
{
    Q_ASSERT(d->state == Private::Stopped);

    d->portReserver = portReserver;
}

void Ice176::setLocalAddresses(const QList<ICE::LocalAddress> &addrs) { d->updateLocalAddresses(addrs); }

void Ice176::setExternalAddresses(const QList<ExternalAddress> &addrs) { d->updateExternalAddresses(addrs); }

void Ice176::setAllowIpExposure(bool enabled) { d->allowIpExposure = enabled; }

void Ice176::setStunDiscoverer(AbstractStunDisco *discoverer)
{
    discoverer->setParent(this);
    d->stunDiscoverer = discoverer;
}

void Ice176::setUseLocal(bool enabled) { d->useLocal = enabled; }

void Ice176::setComponentCount(int count)
{
    Q_ASSERT(d->state == Private::Stopped);

    d->componentCount = count;
}

void Ice176::setLocalFeatures(const Features &features) { d->localFeatures = features; }

void Ice176::setRemoteFeatures(const Features &features) { d->remoteFeatures = features; }

void Ice176::start(Mode mode)
{
    d->mode = mode;
    d->start();
}

void Ice176::stop() { d->stop(); }

bool Ice176::isStopped() const { return d->state == Private::Stopped; }

void Ice176::startChecks() { d->startChecks(); }

QString Ice176::localUfrag() const { return d->localUser; }

QString Ice176::localPassword() const { return d->localPass; }

void Ice176::setRemoteCredentials(const QString &ufrag, const QString &pass)
{
    // TODO detect restart
    d->peerUser = ufrag;
    d->peerPass = pass;
}

void Ice176::addRemoteCandidates(const QList<Candidate> &list) { d->addRemoteCandidates(list); }

void Ice176::setRemoteGatheringComplete()
{
    iceDebug("Got remote gathering complete signal");
    d->setRemoteGatheringComplete();
}

void Ice176::setRemoteSelectedCandidadates(const QList<Ice176::SelectedCandidate> &list)
{
    Q_UNUSED(list);
    // This thing is likely useless since ICE knows exactly which pairs are nominated.
}

bool Ice176::canSendMedia() const { return d->readyToSendMedia; }

bool Ice176::hasPendingDatagrams(int componentIndex) const { return !d->in[componentIndex].isEmpty(); }

QByteArray Ice176::readDatagram(int componentIndex) { return d->in[componentIndex].takeFirst(); }

void Ice176::writeDatagram(int componentIndex, const QByteArray &datagram) { d->write(componentIndex, datagram); }

void Ice176::flagComponentAsLowOverhead(int componentIndex) { d->flagComponentAsLowOverhead(componentIndex); }

void Ice176::changeThread(QThread *thread)
{
    for (auto &c : d->localCandidates) {
        if (c.iceTransport)
            c.iceTransport->changeThread(thread);
    }
    for (auto &p : d->checkList.pairs) {
        if (p->pool)
            p->pool->moveToThread(thread);
    }
    moveToThread(thread);
}

bool Ice176::isLocalGatheringComplete() const { return d->localGatheringComplete; }

bool Ice176::isActive() const { return d->state == Private::Active; }

QList<Ice176::SelectedCandidate> Ice176::selectedCandidates() const
{
    QList<Ice176::SelectedCandidate> ret;
    for (auto const &c : d->components) {
        if (c.selectedPair) {
            const auto &local = c.selectedPair->local;
            ret.append({ local->addr.addr, local->addr.port, local->componentId });
        }
    }
    return ret;
}

QList<ICE::LocalAddress> Ice176::availableNetworkAddresses()
{
    QList<ICE::LocalAddress> listenAddrs;
    auto const               interfaces = QNetworkInterface::allInterfaces();
#ifdef Q_OS_UNIX
    static const auto ignored
        = QStringList { QStringLiteral("vmnet"), QStringLiteral("vnic"), QStringLiteral("vboxnet") };
#endif
    for (const QNetworkInterface &ni : interfaces) {
        if ((ni.flags() & (QNetworkInterface::IsRunning | QNetworkInterface::IsUp))
                != (QNetworkInterface::IsRunning | QNetworkInterface::IsUp)
            || ni.flags() & QNetworkInterface::IsLoopBack
#ifdef Q_OS_UNIX
            || std::any_of(ignored.begin(), ignored.end(), [&ni](auto const &ign) { return ni.name().startsWith(ign); })
#elif defined(Q_OS_WIN)
            || ni.humanReadableName().contains(QStringLiteral("VMnet"))
#endif
        )
            continue;

        QList<QNetworkAddressEntry> entries = ni.addressEntries();
        for (const QNetworkAddressEntry &na : qAsConst(entries)) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
            if (na.preferredLifetime().hasExpired() || na.netmask().isNull())
#else
            if (na.netmask().isNull())
#endif
                continue;

            QHostAddress h = na.ip();
            if (h.isNull() || h.isLoopback()
                || !(h.protocol() == QAbstractSocket::IPv4Protocol || h.protocol() == QAbstractSocket::IPv6Protocol)
                || (h.protocol() == QAbstractSocket::IPv4Protocol && h.toIPv4Address() < 0x01000000))
                continue;

            auto la = ICE::LocalAddress { h, ni.index(), ni.type() };
            // don't put the same address in twice.
            //   this also means that if there are
            //   two link-local ipv6 interfaces
            //   with the exact same address, we
            //   only use the first one
            if (listenAddrs.contains(la))
                continue;

            // TODO review if the next condition is needed (and the above too)
            if (h.protocol() == QAbstractSocket::IPv6Protocol && IpUtil::isLinkLocalAddress(h))
                h.setScopeId(ni.name());
            listenAddrs += la;
        }
    }

    return ICE::LocalAddress::sort(listenAddrs);
}

} // namespace XMPP

#include "ice176.moc"
