/*
 * jignle-ice.cpp - Jingle ICE transport
 * Copyright (C) 2020  Sergey Ilinykh
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

#ifdef JINGLE_SCTP
#include "jingle-sctp.h" //Do not move to avoid warnings with MinGW
#endif

#include "jingle-ice-connection_p.h"
#include "jingle-ice.h"
#include "jingle-ice-udp.h"

#include "dtls.h"
#include "ice176.h"
#include "jingle-session.h"
#include "netnames.h"
#include "stundisco.h"
#include "udpportreserver.h"
#include "xmpp/jid/jid.h"
#include "xmpp_client.h"
#include "xmpp_externalservicediscovery.h"
#include "xmpp_serverinfomanager.h"
#include "xmpp_task.h"

#include <memory>

#include <QElapsedTimer>
#include <QNetworkInterface>
#include <QTimer>

template <class T> constexpr std::add_const_t<T> &as_const(T &t) noexcept { return t; }

namespace XMPP { namespace Jingle { namespace ICE {
    const QString NS(QStringLiteral("urn:xmpp:jingle:transports:ice:0"));
    const QString NS_DTLS(QStringLiteral("urn:xmpp:jingle:apps:dtls:0"));

    static XMPP::Ice176::Candidate elementToCandidate(const QDomElement &e)
    {
        if (e.tagName() != "candidate")
            return XMPP::Ice176::Candidate();

        XMPP::Ice176::Candidate c;
        c.component  = e.attribute("component").toInt();
        c.foundation = e.attribute("foundation");
        c.generation = e.attribute("generation").toInt();
        c.id         = e.attribute("id");
        c.ip         = QHostAddress(e.attribute("ip"));
        c.network    = e.attribute("network").toInt();
        c.port       = e.attribute("port").toInt();
        c.priority   = e.attribute("priority").toInt();
        c.protocol   = e.attribute("protocol");
        c.rel_addr   = QHostAddress(e.attribute("rel-addr"));
        c.rel_port   = e.attribute("rel-port").toInt();
        // TODO: remove this?
        // c.rem_addr = QHostAddress(e.attribute("rem-addr"));
        // c.rem_port = e.attribute("rem-port").toInt();
        c.type = e.attribute("type");

        // TODO tcptype

        return c;
    }

    static QDomElement candidateToElement(QDomDocument *doc, const XMPP::Ice176::Candidate &c)
    {
        QDomElement e = doc->createElement("candidate");
        e.setAttribute("component", QString::number(c.component));
        e.setAttribute("foundation", c.foundation);
        e.setAttribute("generation", QString::number(c.generation));
        if (!c.id.isEmpty())
            e.setAttribute("id", c.id);
        e.setAttribute("ip", c.ip.toString());
        // e.setAttribute("ip", "192.168.0.99");
        if (c.network != -1)
            e.setAttribute("network", QString::number(c.network));
        else // weird?
            e.setAttribute("network", QString::number(0));
        e.setAttribute("port", QString::number(c.port));
        e.setAttribute("priority", QString::number(c.priority));
        e.setAttribute("protocol", c.protocol);
        if (!c.rel_addr.isNull())
            e.setAttribute("rel-addr", c.rel_addr.toString());
        if (c.rel_port != -1)
            e.setAttribute("rel-port", QString::number(c.rel_port));
        // TODO: remove this?
        // if(!c.rem_addr.isNull())
        //    e.setAttribute("rem-addr", c.rem_addr.toString());
        // if(c.rem_port != -1)
        //    e.setAttribute("rem-port", QString::number(c.rem_port));
        e.setAttribute("type", c.type);
        return e;
    }

    QDomElement remoteCandidateToElement(QDomDocument *doc, const Ice176::SelectedCandidate &c)
    {
        auto e = doc->createElement("remote-candidate");
        e.setAttribute(QLatin1String("component"), c.componentId);
        e.setAttribute(QLatin1String("ip"), c.ip.toString());
        e.setAttribute(QLatin1String("port"), c.port);
        return e;
    }

    Ice176::SelectedCandidate elementToRemoteCandidate(const QDomElement &el, bool *ok)
    {
        Ice176::SelectedCandidate c;
        quint16                   tmp;
        tmp = el.attribute(QLatin1String("component")).toUInt(ok);
        *ok = *ok && tmp < 256;
        if (!*ok)
            return c;
        c.componentId = tmp;
        c.ip          = QHostAddress(el.attribute(QLatin1String("ip")));
        c.port        = el.attribute(QLatin1String("port")).toUShort(ok);
        *ok           = *ok && !c.ip.isNull();
        return c;
    }

    struct Element {
        QString           pwd;
        QString           ufrag;
        Dtls::FingerPrint fingerprint;
#ifdef JINGLE_SCTP
        SCTP::MapElement            sctpMap;
        QList<SCTP::ChannelElement> sctpChannels;
#endif
        QList<Ice176::Candidate>         candidates;
        QList<Ice176::SelectedCandidate> remoteCandidates;
        bool                             gatheringComplete = false;

        void cleanupICE()
        {
            candidates.clear();
            remoteCandidates.clear();
            gatheringComplete = false;
        }

        QDomElement toXml(QDomDocument *doc) const
        {
            QDomElement tel = doc->createElementNS(NS, "transport");
            if (!pwd.isEmpty())
                tel.setAttribute(QLatin1String("pwd"), pwd);
            if (!ufrag.isEmpty())
                tel.setAttribute(QLatin1String("ufrag"), ufrag);
            if (fingerprint.isValid())
                tel.appendChild(fingerprint.toXml(doc));
#ifdef JINGLE_SCTP
            if (sctpMap.isValid())
                tel.appendChild(sctpMap.toXml(doc));
            for (auto const &c : sctpChannels)
                tel.appendChild(c.toXml(doc));
#endif
            for (auto const &c : candidates)
                tel.appendChild(candidateToElement(doc, c));
            for (auto const &c : remoteCandidates) {
                tel.appendChild(remoteCandidateToElement(doc, c));
            }
            if (gatheringComplete)
                tel.appendChild(doc->createElement(QLatin1String("gathering-complete")));

            return tel;
        }

        void parse(const QDomElement &el)
        {
            ufrag             = el.attribute(QLatin1String("ufrag"));
            pwd               = el.attribute(QLatin1String("pwd"));
            auto e            = el.firstChildElement(QLatin1String("gathering-complete"));
            gatheringComplete = !e.isNull();

            e = el.firstChildElement(QLatin1String("fingerprint"));
            if (!e.isNull() && !fingerprint.parse(e))
                throw std::runtime_error("invalid fingerprint");

            if (fingerprint.isValid() && !Dtls::isSupported())
                qWarning("Remote requested DTLS but it's not supported by used crypto libraries.");
#ifdef JINGLE_SCTP
            e = el.firstChildElement(QLatin1String("sctpmap"));
            if (!e.isNull() && !sctpMap.parse(e))
                throw std::runtime_error("invalid sctpmap");

            QString chTag(QStringLiteral("channel"));
            for (e = el.firstChildElement(chTag); !e.isNull(); e = e.nextSiblingElement(chTag)) {
                SCTP::ChannelElement channel;
                if (!channel.parse(e))
                    throw std::runtime_error("invalid sctp channel");
                ;
                sctpChannels.append(channel);
            }
#endif
            QString candTag(QStringLiteral("candidate"));
            for (e = el.firstChildElement(candTag); !e.isNull(); e = e.nextSiblingElement(candTag)) {
                auto c = elementToCandidate(e);
                if (!c.component || c.type.isEmpty())
                    throw std::runtime_error("invalid candidate");
                ;
                candidates.append(c);
            }
            if (!candidates.isEmpty()) {
                if (ufrag.isEmpty() || pwd.isEmpty())
                    throw std::runtime_error("user fragment or password can't be empty");
            }

            QString rcTag(QStringLiteral("remote-candidate"));
            for (e = el.firstChildElement(rcTag); !e.isNull(); e = e.nextSiblingElement(rcTag)) {
                bool                      ok;
                Ice176::SelectedCandidate rc = elementToRemoteCandidate(e, &ok);
                if (!ok)
                    throw std::runtime_error("invalid remote candidate");
                ;
                remoteCandidates.append(rc);
            }
        }
    };

    class Resolver : public QObject {
        Q_OBJECT
        using QObject::QObject;

        int                   counter;
        std::function<void()> callback;

        void onOneFinished()
        {
            if (!--counter) {
                callback();
                deleteLater();
            }
        }

    public:
        using ResolveList = std::list<std::pair<QString, std::reference_wrapper<QHostAddress>>>;

        static void resolve(QObject *parent, ResolveList list, std::function<void()> &&callback)
        {
            auto resolver      = new Resolver(parent); // will be deleted when all finished. see onOneFinished
            resolver->counter  = int(list.size());
            resolver->callback = callback;
            for (auto &item : list) {
                // FIXME hosts may dup in the list. needs optimization
                auto *dns = new NameResolver(parent);

                connect(dns, &NameResolver::resultsReady, resolver,
                        [result = item.second, resolver](const QList<XMPP::NameRecord> &records) {
                            result.get() = records.first().address();
                            resolver->onOneFinished();
                        });
                connect(dns, &NameResolver::error, resolver,
                        [resolver](XMPP::NameResolver::Error) { resolver->onOneFinished(); });

                dns->start(item.first.toLatin1());
            }
        }
    };

    class IceStopper : public QObject {
        Q_OBJECT

    public:
        QTimer                 t;
        XMPP::UdpPortReserver *portReserver;
        QList<XMPP::Ice176 *>  left;

        IceStopper(QObject *parent = nullptr) : QObject(parent), t(this), portReserver(nullptr)
        {
            connect(&t, SIGNAL(timeout()), SLOT(t_timeout()));
            t.setSingleShot(true);
        }

        ~IceStopper()
        {
            qDeleteAll(left);
            delete portReserver;
            printf("IceStopper done\n");
        }

        void start(XMPP::UdpPortReserver *_portReserver, const QList<Ice176 *> iceList)
        {
            if (_portReserver) {
                portReserver = _portReserver;
                portReserver->setParent(this);
            }
            left = iceList;

            for (Ice176 *ice : std::as_const(left)) {
                ice->setParent(this);

                // TODO: error() also?
                connect(ice, &Ice176::stopped, this, &IceStopper::ice_stopped);
                connect(ice, &Ice176::error, this, &IceStopper::ice_error);
                ice->stop();
            }

            t.start(3000);
        }

    private slots:
        void ice_stopped()
        {
            XMPP::Ice176 *ice = static_cast<XMPP::Ice176 *>(sender());
            ice->disconnect(this);
            ice->setParent(nullptr);
            ice->deleteLater();
            left.removeAll(ice);
            if (left.isEmpty())
                deleteLater();
        }

        void ice_error(XMPP::Ice176::Error e)
        {
            Q_UNUSED(e)

            ice_stopped();
        }

        void t_timeout() { deleteLater(); }
    };

    class Manager::Private {
    public:
        XMPP::Jingle::Manager *jingleManager = nullptr;

        int          basePort        = -1;
        QString      extHost;
        QHostAddress selfAddr;
        bool         allowIpExposure = true;

        QString stunBindHost;
        int     stunBindPort = 0;
        QString stunRelayUdpHost;
        int     stunRelayUdpPort = 0;
        QString stunRelayUdpUser;
        QString stunRelayUdpPass;
        QString stunRelayTcpHost;
        int     stunRelayTcpPort = 0;
        QString stunRelayTcpUser;
        QString stunRelayTcpPass;

        XMPP::TurnClient::Proxy stunProxy;

        // FIMME it's reuiqred to split transports by direction otherwise we gonna hit conflicts.
        // jid,transport-sid -> transport mapping
        //        QSet<QPair<Jid, QString>>   sids;
        //        QHash<QString, Transport *> key2transport;
        //        Jid                         proxy;
    };

    class RawConnection : public XMPP::Jingle::Connection {
        Q_OBJECT
    public:
        using Ptr = QSharedPointer<RawConnection>;

        enum DisconnectReason { None, DtlsClosed };

        QList<QNetworkDatagram> datagrams;
        DisconnectReason        disconnectReason = None;
        quint8                  componentIndex   = 0;

        RawConnection(quint8 componentIndex) : componentIndex(componentIndex) { };

        int component() const override { return componentIndex; }

        TransportFeatures features() const override
        {
            return TransportFeature::Fast | TransportFeature::MessageOriented | TransportFeature::HighProbableConnect
                | TransportFeature::Unreliable;
        }

        bool hasPendingDatagrams() const override { return datagrams.size() > 0; }

        QNetworkDatagram readDatagram(qint64 maxSize = -1) override
        {
            Q_UNUSED(maxSize) // TODO or not?
            return datagrams.size() ? datagrams.takeFirst() : QNetworkDatagram();
        }

        qint64 bytesAvailable() const override { return 0; }

        qint64 bytesToWrite() const override { return 0; /*client->bytesToWrite();*/ }

        void close() override { XMPP::Jingle::Connection::close(); }

        qint64 readDataInternal(char *, qint64) override
        {
            qWarning("jingle-ice: called unsupported RawConnection::readDataInternal");
            return -1;
        }

    private:
        friend class Transport;

        void onConnected()
        {
            qDebug("jingle-ice: channel connected!");
            emit connected();
        }

        void onError(QAbstractSocket::SocketError error) { qDebug("jingle-ice: channel failed: %d", error); }

        void onDisconnected(DisconnectReason reason)
        {
            if (!isOpen())
                return;
            disconnectReason = reason;
            setOpenMode(QIODevice::ReadOnly);
            emit disconnected();
        }

        void enqueueIncomingUDP(const QByteArray &data)
        {
            datagrams.append(QNetworkDatagram { data });
            emit readyRead();
        }
    };

    IceConnection::~IceConnection()
    {
        // Resource destruction must not re-enter packet routing while another
        // component is already being torn down.
        if (ice)
            ice->disconnect(this);
        for (const auto &c : components) {
            if (c.dtls)
                c.dtls->disconnect(this);
#ifdef JINGLE_SCTP
            if (c.sctp)
                c.sctp->disconnect(this);
#endif
        }
        // Tear down packet consumers before handing ICE to asynchronous shutdown.
        for (auto &c : components) {
            delete c.srtp;
#ifdef JINGLE_SCTP
            delete c.sctp;
#endif
            delete c.dtls;
        }
        if (ice) {
            auto stopper = new IceStopper;
            stopper->start(portReserver, QList<Ice176 *>() << ice);
        }
    }

    class Transport::Private {
    public:
        enum PendingActions {
            NewCandidate       = 1,
            RemoteCandidate    = 2,
            GatheringComplete  = 4,
            NewFingerprint     = 8,
            NewSctpAssociation = 16
        };

        Transport                     *q                         = nullptr;
        bool                           offerSent                 = false;
        bool                           aborted                   = false;
        bool                           initialOfferReady         = false;
        bool                           remoteAcceptedFingerprint = false;
        quint16                        pendingActions            = 0;
        int                            proxiesInDiscoCount       = 0;
        QList<XMPP::Ice176::Candidate> pendingLocalCandidates; // cid to candidate mapping
        std::unique_ptr<Element>       remoteState;

        // QString            sid;
        // Transport::Mode    mode = Transport::Tcp;
        // QTimer             probingTimer;
        // QTimer             negotiationFinishTimer;
        // QElapsedTimer      lastConnectionStart;
        // size_t             blockSize    = 8192;
        TcpPortDiscoverer *disco = nullptr;
        Resolver           resolver;

        Dtls::Setup localDtlsRole  = Dtls::ActPass;
        Dtls::Setup remoteDtlsRole = Dtls::ActPass;
#ifdef JINGLE_SCTP
        SCTP::MapElement sctp;
#endif

        QHostAddress extAddr;
        QHostAddress stunBindAddr, stunRelayUdpAddr, stunRelayTcpAddr;
        int          stunBindPort;
        int          stunRelayUdpPort;
        int          stunRelayTcpPort;
        QString      stunRelayUdpUser;
        QString      stunRelayUdpPass;
        QString      stunRelayTcpUser;
        QString      stunRelayTcpPass;
        // QString

        // udp stuff
        bool         udpInitialized;
        quint16      udpPort;
        QHostAddress udpAddress;

        // One membership for now. Group sharing is enabled only after signaling and
        // callbacks no longer depend on a single content's Transport.
        QSharedPointer<IceConnection> network;
        QStringList                   rtpProfiles;

        ~Private()
        {
            // No callback capturing this Private may survive its destruction.
            if (network->ice)
                network->ice->disconnect(q);
            for (const auto &c : network->components) {
                if (c.dtls)
                    c.dtls->disconnect(q);
#ifdef JINGLE_SCTP
                if (c.sctp)
                    c.sctp->disconnect(q);
#endif
            }
        }

        inline Jid remoteJid() const { return q->_pad->session()->peer(); }

        Component &addComponent()
        {
            network->components.append(Component {});
            auto &c          = network->components.last();
            c.componentIndex = network->components.size() - 1;
            return c;
        }

        bool setupDtls(int componentIndex)
        {
            qDebug("Setup DTLS");
            Q_ASSERT(componentIndex < network->components.length());
            if (network->components[componentIndex].dtls)
                return true;
            network->components[componentIndex].dtls
                = new Dtls(network.data(), q->pad()->session()->me().full(), q->pad()->session()->peer().full());

            auto dtls = network->components[componentIndex].dtls;
            if (!rtpProfiles.isEmpty()) {
                if (!dtls->setSRTPProfiles(rtpProfiles)) {
                    return false;
                }
                network->components[componentIndex].srtp = new RTP::SrtpSession(dtls, network.data());
            }
            if (q->isLocal()) {
                dtls->initOutgoing();
            } else {
                dtls->setRemoteFingerprint(remoteState->fingerprint);
                dtls->acceptIncoming();
            }

            if (componentIndex == 0) { // for other components it's the same but we don't need multiple fingerprints
                dtls->connect(
                    dtls, &Dtls::needRestart, q,
                    [this]() {
                        pendingActions |= NewFingerprint;
                        remoteAcceptedFingerprint = false;
                        emit q->updated();
                    },
                    Qt::QueuedConnection);
                pendingActions |= NewFingerprint;
            }
            dtls->connect(dtls, &Dtls::readyRead, network.data(), [net = network.data(), componentIndex]() {
                auto &component = net->components[componentIndex];
                auto  d         = component.dtls->readDatagram();
#ifdef JINGLE_SCTP
                if (component.sctp) {
                    // qDebug("sctp write incoming");
                    component.sctp->writeIncoming(d);
                }
#endif
            });
            dtls->connect(dtls, &Dtls::readyReadOutgoing, network.data(), [net = network.data(), componentIndex]() {
                net->ice->writeDatagram(componentIndex, net->components[componentIndex].dtls->readOutgoingDatagram());
            });
            dtls->connect(dtls, &Dtls::connected, network.data(), [net = network.data(), componentIndex, dtls]() {
                qDebug("Dtls::connected");
                auto &c = net->components[componentIndex];
#ifdef JINGLE_SCTP
                if (c.sctp) {
                    // see rfc8864 (6.1) and rfc8832 (6)
                    c.sctp->setIdSelector(dtls->localFingerprint().setup == Dtls::Active ? SCTP::IdSelector::Even
                                                                                         : SCTP::IdSelector::Odd);
                    c.sctp->onTransportConnected();
                }
#endif
                if (c.rawConnection)
                    c.rawConnection->onConnected();
            });
            dtls->connect(dtls, &Dtls::errorOccurred, network.data(),
                          [net = network.data(), componentIndex](QAbstractSocket::SocketError error) {
                              qDebug("dtls failed for component %d", componentIndex);
                              auto &c = net->components[componentIndex];
#ifdef JINGLE_SCTP
                              if (c.sctp)
                                  c.sctp->onTransportError(error);
#endif
                              if (c.rawConnection)
                                  c.rawConnection->onError(error);
                          });
            dtls->connect(dtls, &Dtls::closed, network.data(), [net = network.data(), componentIndex]() {
                qDebug("dtls closed for component %d", componentIndex);
                auto &c = net->components[componentIndex];
                if (c.rawConnection)
                    c.rawConnection->onDisconnected(RawConnection::DtlsClosed);
#ifdef JINGLE_SCTP
                if (c.sctp)
                    c.sctp->onTransportClosed();
#endif
            });
            return true;
        }

        void findStunAndTurn()
        {
            auto extDisco = q->pad()->session()->manager()->client()->externalServiceDiscovery();
            using namespace std::chrono_literals;
            if (extDisco->isSupported()) {
                extDisco->services(q,
                                   [this](const ExternalServiceList &services) {
                                       ExternalService::Ptr stun;
                                       ExternalService::Ptr turnUdp;
                                       ExternalService::Ptr turnTcp;
                                       for (auto const &s : std::as_const(services)) {
                                           if (s->type == QLatin1String("stun")
                                               && (s->transport.isEmpty() || s->transport == QLatin1String("udp")))
                                               stun = s;
                                           else if (s->type == QLatin1String("turn")) {
                                               if (s->transport == QLatin1String("tcp"))
                                                   turnTcp = s;
                                               else
                                                   turnUdp = s;
                                           }
                                       }
                                       Resolver::ResolveList resList;
                                       if (stun) {
                                           stunBindAddr.setAddress(stun->host);
                                           stunBindPort = stun->port;
                                           if (stunBindAddr.isNull())
                                               resList.emplace_back(stun->host, std::ref(stunBindAddr));
                                       }
                                       if (turnTcp) {
                                           stunRelayTcpAddr.setAddress(turnTcp->host);
                                           stunRelayTcpPort = turnTcp->port;
                                           stunRelayTcpUser = turnTcp->username;
                                           stunRelayTcpPass = turnTcp->password;
                                           if (stunRelayTcpAddr.isNull())
                                               resList.emplace_back(turnTcp->host, std::ref(stunRelayTcpAddr));
                                       }
                                       if (turnUdp) {
                                           stunRelayUdpAddr.setAddress(turnUdp->host);
                                           stunRelayUdpPort = turnUdp->port;
                                           stunRelayUdpUser = turnUdp->username;
                                           stunRelayUdpPass = turnUdp->password;
                                           if (stunRelayUdpAddr.isNull())
                                               resList.emplace_back(turnUdp->host, std::ref(stunRelayUdpAddr));
                                       }
                                       if (resList.empty()) {
                                           startIce();
                                       } else {
                                           Resolver::resolve(q, resList, [this]() {
                                               qDebug("resolver finished");
                                               startIce();
                                           });
                                       }
                                   },
                                   5min, { "stun", "turn" });
                return;
            }

            auto manager     = dynamic_cast<Manager *>(q->pad()->manager())->d.get();
            stunBindPort     = manager->stunBindPort;
            stunRelayUdpPort = manager->stunRelayUdpPort;
            stunRelayTcpPort = manager->stunRelayTcpPort;
            stunRelayUdpUser = manager->stunRelayUdpUser;
            stunRelayUdpPass = manager->stunRelayUdpPass;
            stunRelayTcpUser = manager->stunRelayTcpUser;
            stunRelayTcpPass = manager->stunRelayTcpPass;
            Resolver::resolve(q,
                              { { manager->extHost, std::ref(extAddr) },
                                { manager->stunBindHost, std::ref(stunBindAddr) },
                                { manager->stunRelayUdpHost, std::ref(stunRelayUdpAddr) },
                                { manager->stunRelayTcpHost, std::ref(stunRelayTcpAddr) } },
                              [this]() {
                                  qDebug("resolver finished");
                                  startIce();
                              });
        }

        void startIce()
        {
            auto manager = dynamic_cast<Manager *>(q->_pad->manager())->d.get();

            if (!stunBindAddr.isNull() && stunBindPort > 0)
                qDebug("STUN service: %s;%d", qPrintable(stunBindAddr.toString()), stunBindPort);
            if (!stunRelayUdpAddr.isNull() && stunRelayUdpPort > 0 && !stunRelayUdpUser.isEmpty())
                qDebug("TURN w/ UDP service: %s;%d", qPrintable(stunRelayUdpAddr.toString()), stunRelayUdpPort);
            if (!stunRelayTcpAddr.isNull() && stunRelayTcpPort > 0 && !stunRelayTcpUser.isEmpty())
                qDebug("TURN w/ TCP service: %s;%d", qPrintable(stunRelayTcpAddr.toString()), stunRelayTcpPort);

            auto listenAddrs = manager->selfAddr.isNull() ? Ice176::availableNetworkAddresses()
                                                          : QList<QHostAddress> { manager->selfAddr };

            QList<XMPP::Ice176::LocalAddress> localAddrs;

            QStringList strList;
            for (const QHostAddress &h : as_const(listenAddrs)) {
                XMPP::Ice176::LocalAddress addr;
                addr.addr = h;
                localAddrs += addr;
                strList += h.toString();
            }

            if (manager->basePort != -1) {
                network->portReserver = new XMPP::UdpPortReserver(network.data());
                network->portReserver->setAddresses(listenAddrs);
                network->portReserver->setPorts(manager->basePort, 4);
            }

            if (!strList.isEmpty()) {
                printf("Host addresses:\n");
                for (const QString &s : as_const(strList))
                    printf("  %s\n", qPrintable(s));
            }

            network->ice = new Ice176(network.data());
            network->ice->setAllowIpExposure(manager->allowIpExposure);

            q->connect(network->ice, &XMPP::Ice176::started, q, [this]() {
                for (auto const &c : as_const(network->components)) {
                    if (c.lowOverhead)
                        network->ice->flagComponentAsLowOverhead(c.componentIndex);
                }
            });
            q->connect(network->ice, &XMPP::Ice176::error, q, [this](XMPP::Ice176::Error err) {
                q->onFinish(Reason::Condition::ConnectivityError, QString("ICE failed: %1").arg(err));
            });
            q->connect(network->ice, &XMPP::Ice176::localCandidatesReady, q,
                       [this](const QList<XMPP::Ice176::Candidate> &candidates) {
                           pendingActions |= NewCandidate;
                           pendingLocalCandidates += candidates;
                           qDebug("discovered %lld local candidates", qsizetype(candidates.size()));
                           for (auto const &c : candidates) {
                               qDebug(" - %s:%d", qPrintable(c.ip.toString()), c.port);
                           }
                           emit q->updated();
                       });
            q->connect(network->ice, &XMPP::Ice176::localGatheringComplete, q, [this]() {
                if (q->pad()->ns() == NS)
                    pendingActions |= GatheringComplete;
                if (pendingActions)
                    emit q->updated();
            });
            q->connect(
                network->ice, &XMPP::Ice176::readyToSendMedia, q,
                [this]() {
                    qDebug("ICE reported ready to send media!");
                    if (!network->components[0].dtls) // if empty
                        notifyRawConnected();
                    else if (remoteAcceptedFingerprint)
                        for (const auto &c : as_const(network->components))
                            c.dtls->onRemoteAcceptedFingerprint();
                },
                Qt::QueuedConnection); // signal is not DOR-SS
            QObject::connect(network->ice, &Ice176::readyRead, network.data(),
                             [net = network.data()](int componentIndex) {
                                 auto  buf       = net->ice->readDatagram(componentIndex);
                                 auto &component = net->components[componentIndex];
                                 if (component.srtp) {
                                     component.srtp->dispatchMuxed(std::move(buf));
                                 } else if (component.dtls) {
                                     component.dtls->writeIncomingDatagram(buf);
                                 } else if (component.rawConnection) {
                                     component.rawConnection->enqueueIncomingUDP(buf);
                                 }
                             });

            network->ice->setProxy(manager->stunProxy);
            if (network->portReserver)
                network->ice->setPortReserver(network->portReserver);

            // QList<XMPP::Ice176::LocalAddress> localAddrs;
            // XMPP::Ice176::LocalAddress addr;

            // FIXME: the following is not true, a local address is not
            //   required, for example if you use TURN with TCP only

            // a local address is required to use ice.  however, if
            //   we don't have a local address, we won't handle it as
            //   an error here.  instead, we'll start Ice176 anyway,
            //   which should immediately error back at us.
            /*if(manager->selfAddr.isNull())
            {
                printf("no self address to use.  this will fail.\n");
                return;
            }

            addr.addr = manager->selfAddr;
            localAddrs += addr;*/
            network->ice->setLocalAddresses(localAddrs);

            // if an external address is manually provided, then apply
            //   it only to the selfAddr.  FIXME: maybe we should apply
            //   it to all local addresses?
            if (!extAddr.isNull()) {
                QList<XMPP::Ice176::ExternalAddress> extAddrs;
                /*XMPP::Ice176::ExternalAddress eaddr;
                eaddr.base = addr;
                eaddr.addr = extAddr;
                extAddrs += eaddr;*/
                for (const XMPP::Ice176::LocalAddress &la : as_const(localAddrs)) {
                    XMPP::Ice176::ExternalAddress ea;
                    ea.base = la;
                    ea.addr = extAddr;
                    extAddrs += ea;
                }
                network->ice->setExternalAddresses(extAddrs);
            }

            if (!stunBindAddr.isNull() && stunBindPort > 0)
                network->ice->setStunBindService(stunBindAddr, stunBindPort);
            if (!stunRelayUdpAddr.isNull() && !stunRelayUdpUser.isEmpty())
                network->ice->setStunRelayUdpService(stunRelayUdpAddr, stunRelayUdpPort, stunRelayUdpUser,
                                                     stunRelayUdpPass.toUtf8());
            if (!stunRelayTcpAddr.isNull() && !stunRelayTcpUser.isEmpty())
                network->ice->setStunRelayTcpService(stunRelayTcpAddr, stunRelayTcpPort, stunRelayTcpUser,
                                                     stunRelayTcpPass.toUtf8());
            network->ice->setStunDiscoverer(
                q->pad()->session()->manager()->client()->stunDiscoManager()->createMonitor());

            network->ice->setComponentCount(network->components.count());
            network->ice->setLocalFeatures(Ice176::Trickle);

            setupRemoteICE(*remoteState);
            remoteState->cleanupICE();

            auto mode = q->creator() == q->pad()->session()->role() ? XMPP::Ice176::Initiator : XMPP::Ice176::Responder;
            network->ice->start(mode);
        }

        void setupRemoteICE(const Element &e)
        {
            Q_ASSERT(network->ice != nullptr);
            if (!e.candidates.isEmpty()) {
                network->ice->setRemoteCredentials(e.ufrag, e.pwd);
                network->ice->addRemoteCandidates(e.candidates);
            }
            if (e.gatheringComplete) {
                network->ice->setRemoteGatheringComplete();
            }
            if (e.remoteCandidates.size()) {
                network->ice->setRemoteSelectedCandidadates(e.remoteCandidates);
            }
        }

        void handleRemoteUpdate(const Element &e)
        {
            if (q->state() == State::Finished)
                return;

            if (network->ice) {
                setupRemoteICE(e);
            } else {
                if (!e.candidates.isEmpty() || !e.ufrag.isEmpty()) {
                    remoteState->ufrag = e.ufrag;
                    remoteState->pwd   = e.pwd;
                }

                if (e.gatheringComplete) {
                    remoteState->gatheringComplete = true;
                }
                remoteState->candidates += e.candidates;
                if (e.remoteCandidates.size())
                    remoteState->remoteCandidates = e.remoteCandidates;
            }
            if (e.fingerprint.isValid()) {
                remoteState->fingerprint = e.fingerprint;
                if (q->isLocal()) {
                    // dtls already created for local transport on remote accept
                    for (auto &c : network->components) {
                        if (c.dtls)
                            c.dtls->setRemoteFingerprint(e.fingerprint);
                    }
                }
            }
#ifdef JINGLE_SCTP
            if (e.sctpMap.isValid()) {
                remoteState->sctpMap = e.sctpMap;
                remoteState->sctpChannels += e.sctpChannels;
            }
#endif
            if (q->state() == State::Created && q->isRemote()) {
                // initial incoming transport
                q->setState(State::Pending);
            }
            if (q->state() == State::Pending && q->isLocal()) {
                // initial acceptance by remote of the local transport
                q->setState(State::Accepted);
            }
        }

        bool isDataChannelSuppported() const { return Dtls::isSupported(); }

        void notifyRawConnected()
        {
            auto const &acceptors = q->acceptors();
            for (auto const &acceptor : acceptors) {
                if (!(acceptor.features & TransportFeature::DataOriented))
                    ensureRawConnection(acceptor.componentIndex); // TODO signals
            }
            for (auto &c : network->components) {
                if (c.rawConnection)
                    c.rawConnection->onConnected();
            }
        }

        /**
         * @brief ensureComponentExist adds an ICE component if missed and it's allowed atm for a channel
         *        with given featuers.
         * @param componentIndex  - desired component index (-1 for auto)
         * @return actually allocated component index (e.g. when -1 passed).
         *         returns -1 if something went wrong.
         */
        int ensureComponentExist(int componentIndex, bool lowOverhead = false)
        {
            if (!rtpProfiles.isEmpty()) {
                if (componentIndex > 0)
                    return -1;
                componentIndex = 0;
            }
            if (componentIndex == -1) {
                for (auto &c : network->components)
                    if (!c.initialized) {
                        componentIndex = c.componentIndex;
                        break;
                    }
                if (componentIndex < 0)
                    componentIndex = network->components.count();
            }

            if (componentIndex >= network->components.size()) {
                if (network->ice) {
                    qWarning("Adding channel after negotiation start is not yet supported");
                    return -1;
                }
                for (int i = network->components.size(); i < componentIndex + 1; i++) {
                    addComponent();
                }
            }
            auto &c       = network->components[componentIndex];
            c.initialized = true;
            if (lowOverhead)
                c.lowOverhead = true;
            return componentIndex;
        }

        void ensureRawConnection(int componentIndex)
        {
            auto index = quint8(componentIndex);
            if (network->components[index].rawConnection)
                return;
            network->components[index].rawConnection = RawConnection::Ptr::create(index);
            if (q->isRemote() && !q->notifyIncomingConnection(network->components[index].rawConnection))
                network->components[index].rawConnection.reset();
            // Do we need anything else to do here? connect signals for example?
        }
#ifdef JINGLE_SCTP
        void initSctpAssociation(int componentIndex)
        {
            auto &c = network->components[componentIndex];
            Q_ASSERT(c.sctp == nullptr);
            c.sctp = new SCTP::Association(network.data());
            pendingActions |= NewSctpAssociation;
            if (q->wasAccepted() && q->state() != State::ApprovedToSend) // like we already sent our decision
                emit q->updated();
            if (remoteState->sctpMap.isValid()) {
                // TODO if we already have associations params try to ruse them instead of making new one
            }
            QObject::connect(c.sctp, &SCTP::Association::readyReadOutgoing, network.data(),
                             [net = network.data(), componentIndex]() {
                                 auto &c   = net->components[componentIndex];
                                 auto  buf = c.sctp->readOutgoing();
                                 c.dtls->writeDatagram(buf);
                             });
            q->connect(c.sctp, &SCTP::Association::newIncomingChannel, q, [this, componentIndex]() {
                qDebug("new incoming sctp channel");
                auto assoc   = network->components[componentIndex].sctp;
                auto channel = assoc->nextChannel();
                if (!q->notifyIncomingConnection(channel)) {
                    channel->close();
                }
            });
        }

        Connection::Ptr addDataChannel(TransportFeatures channelFeatures, const QString &label, int &componentIndex)
        {
            if (componentIndex == -1)
                componentIndex = 0;

            if (!Dtls::isSupported()) {
                qWarning("An attempt to add a data channel while DTLS is not supported by current configuration");
                return {};
            }
            componentIndex = ensureComponentExist(componentIndex, channelFeatures & TransportFeature::LowOverhead);
            if (componentIndex < 0)
                return {}; // failed to add component for the features

            auto &c = network->components[componentIndex];
            if (!c.sctp) {
                // basically we can't accept remote transport with own stcp if it wasn't offered.
                // but we can add new associations later with transport-info (undocumented in xep)
                if (q->isRemote() && !q->wasAccepted() && !(remoteState && remoteState->sctpMap.isValid())) {
                    qWarning("remote hasn't negotiated sctp association");
                    return {};
                }
                initSctpAssociation(componentIndex);
            }
            Q_UNUSED(channelFeatures); // TODO
            return c.sctp->newChannel(SCTP::Reliable, true, 0, 256, label);
        }
#endif
    };

    Transport::Transport(const TransportManagerPad::Ptr &pad, Origin creator) :
        XMPP::Jingle::Transport(pad, creator), d(new Private)
    {
        d->q       = this;
        d->network = pad.staticCast<Pad>()->connectionFor(this);
        d->ensureComponentExist(0);
        d->remoteState.reset(new Element {});
        connect(this, &XMPP::Jingle::Transport::stateChanged, this, [this]() {
            if (_state >= State::Finishing) {
                if (auto binding = rtpSession())
                    binding->close();
            }
        });
        connect(this, &XMPP::Jingle::Transport::failed, this, [this]() {
            if (auto binding = rtpSession())
                binding->close();
        });
        connect(_pad->manager(), &TransportManager::abortAllRequested, this, [this]() {
            d->aborted = true;
            onFinish(Reason::Cancel);
        });
    }

    Transport::~Transport()
    {
        if (auto binding = rtpSession()) {
            binding->disconnect();
            binding->close();
        }
    }

    void Transport::stop()
    {
        XMPP::Jingle::Transport::stop();
        if (auto binding = rtpSession())
            binding->close();
    }

    bool Transport::enableRtpMux()
    {
        if ((_state != State::Created && !(isRemote() && _state == State::Pending)) || d->network->ice
            || d->network->components.size() != 1 || d->network->components[0].dtls
            || d->network->components[0].rawConnection)
            return false;
        auto       profiles     = RTP::SrtpContext::supportedProfiles();
        const auto dtlsProfiles = Dtls::supportedSRTPProfiles();
        for (auto it = profiles.begin(); it != profiles.end();) {
            if (!dtlsProfiles.contains(*it))
                it = profiles.erase(it);
            else
                ++it;
        }
        if (profiles.isEmpty())
            return false;
        d->rtpProfiles                        = profiles;
        d->network->components[0].lowOverhead = true;
        return true;
    }

    RTP::SrtpSession *Transport::rtpSession() const
    {
        return d->network->components.isEmpty() ? nullptr : d->network->components[0].srtp;
    }

    bool Transport::sendRtpPacket(QByteArray packet, RTP::SrtpContext::Packet kind, quint64 epoch)
    {
        auto binding = rtpSession();
        if (!binding || !d->network->ice || _state < State::Connecting || _state >= State::Finishing)
            return false;
        auto encrypted = binding->protectMuxed(std::move(packet), kind, epoch);
        if (!encrypted)
            return false;
        d->network->ice->writeDatagram(0, *encrypted);
        return true;
    }

    void Transport::prepare()
    {
        qDebug("Prepare local offer");
        if (!d->rtpProfiles.isEmpty() && isRemote() && !d->remoteState->fingerprint.isValid()) {
            onFinish(Reason::SecurityError, QStringLiteral("RTP requires a DTLS fingerprint"));
            return;
        }
        QPointer<Transport> guard(this);
        setState(State::ApprovedToSend);
        if (!guard || _state != State::ApprovedToSend)
            return;
        auto const &a = acceptors();
        for (auto const &acceptor : a) {
            if (!d->rtpProfiles.isEmpty() && !(acceptor.features & TransportFeature::DataOriented)) {
                onFinish(Reason::FailedTransport, QStringLiteral("Raw channels are not allowed on protected RTP"));
                return;
            }
            int ci = acceptor.componentIndex < 0 ? 0 : acceptor.componentIndex;
            if (d->ensureComponentExist(ci, acceptor.features & TransportFeature::LowOverhead) < 0) {
                onFinish(Reason::FailedTransport, QStringLiteral("Incompatible ICE component"));
                return;
            }
            if (acceptor.features & TransportFeature::DataOriented)
                d->network->components[ci].needDatachannel = true;
        }

        if (Dtls::isSupported()
            && ((!d->rtpProfiles.isEmpty()) || (isLocal() && _pad->session()->checkPeerCaps(Dtls::FingerPrint::ns()))
                || (isRemote() && d->remoteState->fingerprint.isValid()))) {
            qDebug("initialize DTLS");

            for (auto &c : d->network->components) {
                if (!d->setupDtls(c.componentIndex)) {
                    onFinish(Reason::SecurityError, QStringLiteral("DTLS-SRTP profile configuration failed"));
                    return;
                }
#ifdef JINGLE_SCTP
                if (isRemote() && c.needDatachannel && !c.sctp) {
                    d->initSctpAssociation(c.componentIndex);
                }
#endif
            }
        }

        d->findStunAndTurn();
        emit updated();
    }

    // we got content acceptance from any side and now can connect
    void Transport::start()
    {
        qDebug("Starting connecting");
        if (_state >= State::Finishing)
            return;
        if (!d->network->ice) {
            onFinish(Reason::FailedTransport, QStringLiteral("ICE transport has not been prepared"));
            return;
        }
        QPointer<Transport> guard(this);
        setState(State::Connecting);
        if (guard && _state == State::Connecting)
            d->network->ice->startChecks();
    }

    bool Transport::update(const QDomElement &transportEl)
    {
        try {
            QDomDocument normalizedDoc;
            QDomElement  normalized = transportEl;
            if (_pad->ns() == NS_ICE_UDP) {
                QString error;
                normalized = iceUdpToInternal(normalizedDoc, transportEl, NS, &error);
                if (normalized.isNull()) {
                    qWarning("ICE-UDP transport update failed: %s", qPrintable(error));
                    return false;
                }
            }
            Element e;
            e.parse(normalized);
            QTimer::singleShot(0, this, [this, e]() { d->handleRemoteUpdate(e); });
            return true;
        } catch (std::runtime_error &e) {
            qWarning("Transport update failed: %s", e.what());
            return false;
        }
    }

    bool Transport::hasUpdates() const
    {
        return isValid() && d->pendingActions && d->network->ice && _state >= State::ApprovedToSend
            && !(isRemote() && _state == State::Pending)
            && (d->network->ice->isLocalGatheringComplete() || d->pendingLocalCandidates.size());
    }

    OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate([[maybe_unused]] bool ensureTransportElement = false)
    {
        if (!hasUpdates()) {
            return {};
        }

        qDebug("jingle-ice: taking outgoing update");
        Element e;
        e.ufrag = d->network->ice->localUfrag();
        e.pwd   = d->network->ice->localPassword();

        bool hasFingerprint = d->pendingActions & Private::NewFingerprint;
        if (hasFingerprint && d->network->components[0].dtls) {
            e.fingerprint = d->network->components[0].dtls->localFingerprint();
        }
        e.candidates        = d->pendingLocalCandidates;
        e.gatheringComplete = d->pendingActions & Private::GatheringComplete;
        if (d->pendingActions & Private::RemoteCandidate)
            e.remoteCandidates = d->network->ice->selectedCandidates();
        // TODO sctp

        auto        doc          = _pad.staticCast<Pad>()->session()->manager()->client()->doc();
        QDomElement transportXml = e.toXml(doc);
        if (_pad->ns() == NS_ICE_UDP) {
            QString error;
            transportXml = internalToIceUdp(*doc, transportXml, &error);
            if (transportXml.isNull()) {
                qWarning("Failed to serialize ICE-UDP transport update: %s", qPrintable(error));
                return {};
            }
        }

        d->pendingLocalCandidates.clear();
        d->pendingActions = 0;

        return OutgoingTransportInfoUpdate { transportXml,
                                             [this, trptr = QPointer<Transport>(d->q), hasFingerprint](Task *task) {
                                                 if (!trptr || !task || !task->success())
                                                     return;
                                                 // if we send our fingerprint as a response to remotely initiated dtls
                                                 // then on response we are sure remote server started dtls server and
                                                 // we can connect now.
                                                 if (hasFingerprint) {
                                                     d->remoteAcceptedFingerprint = true;
                                                 }
                                                 if (hasFingerprint && d->network->ice
                                                     && d->network->ice->canSendMedia())
                                                     for (auto &c : d->network->components)
                                                         c.dtls->onRemoteAcceptedFingerprint();
                                             } };
    }

    bool Transport::isValid() const { return d != nullptr; }

    TransportFeatures Transport::features() const { return _pad->manager()->features(); }

    int Transport::maxSupportedChannelsPerComponent(TransportFeatures features) const
    {
        return features & TransportFeature::DataOriented ? 65536 : 1;
    };

    void Transport::setComponentsCount(int count)
    {
        if (!d->rtpProfiles.isEmpty() && count != 1)
            return; // This mode has explicitly negotiated RTCP multiplexing.
        if (_state >= State::ApprovedToSend) {
            qWarning("adding component after ICE started is not supported");
            return;
        }
        for (int i = d->network->components.size(); i < count; i++) {
            d->addComponent();
        }
    }

    // adding ice channels/components (for rtp, rtcp, datachannel etc)
    Connection::Ptr Transport::addChannel(TransportFeatures features, const QString &id, int componentIndex)
    {
#ifdef JINGLE_SCTP
        if (features & TransportFeature::DataOriented)
            return d->addDataChannel(features, id, componentIndex);
#endif
        if (!d->rtpProfiles.isEmpty())
            return {}; // Protected RTP uses sendRtpPacket, never a raw channel.
        componentIndex = d->ensureComponentExist(componentIndex, features & TransportFeature::LowOverhead);
        if (componentIndex < 0)
            return {}; // failed to add component for the features
        d->ensureRawConnection(componentIndex);
        auto &channel = d->network->components[componentIndex].rawConnection;
        channel->setId(id);
        return channel;
    }

    QList<XMPP::Jingle::Connection::Ptr> Transport::channels() const
    {
        QList<XMPP::Jingle::Connection::Ptr> ret;
        for (auto &c : d->network->components) {
            if (c.rawConnection)
                ret.append(c.rawConnection.staticCast<XMPP::Jingle::Connection>());
#ifdef JINGLE_SCTP
            if (c.sctp)
                ret += c.sctp->channels();
#endif
        }
        return ret;
    }

    //----------------------------------------------------------------
    // Manager
    //----------------------------------------------------------------
    Manager::Manager(QObject *parent) : TransportManager(parent), d(new Private) { }

    Manager::~Manager()
    {
        if (d->jingleManager) {
            d->jingleManager->unregisterTransport(NS_ICE_UDP);
            d->jingleManager->unregisterTransport(NS);
        }
    }

    TransportFeatures Manager::features() const
    {
        return TransportFeature::HighProbableConnect | TransportFeature::Reliable | TransportFeature::Unreliable
            | TransportFeature::MessageOriented | TransportFeature::LiveOriented
#ifdef JINGLE_SCTP
            | (Dtls::isSupported() ? (TransportFeature::DataOriented | TransportFeature::Ordered) : TransportFeature(0))
#endif
            ;
    }

    void Manager::setJingleManager(XMPP::Jingle::Manager *jm) { d->jingleManager = jm; }

    QSharedPointer<XMPP::Jingle::Transport> Manager::newTransport(const TransportManagerPad::Ptr &pad, Origin creator)
    {
        return QSharedPointer<Transport>::create(pad, creator).staticCast<XMPP::Jingle::Transport>();
    }

    TransportManagerPad *Manager::pad(Session *session) { return new Pad(this, session); }

    QStringList Manager::ns() const { return { NS, NS_ICE_UDP }; }
    QStringList Manager::discoFeatures() const
    {
        return { NS, NS_ICE_UDP, NS_DTLS
#ifdef JINGLE_SCTP
                 ,
                 SCTP::ns()
#endif
        };
    }

    void Manager::setBasePort(int port) { d->basePort = port; }

    void Manager::setExternalAddress(const QString &host) { d->extHost = host; }

    void Manager::setSelfAddress(const QHostAddress &addr) { d->selfAddr = addr; }

    void Manager::setAllowIpExposure(bool allow) { d->allowIpExposure = allow; }

    void Manager::setStunBindService(const QString &host, int port)
    {
        d->stunBindHost = host;
        d->stunBindPort = port;
    }

    void Manager::setStunRelayUdpService(const QString &host, int port, const QString &user, const QString &pass)
    {
        d->stunRelayUdpHost = host;
        d->stunRelayUdpPort = port;
        d->stunRelayUdpUser = user;
        d->stunRelayUdpPass = pass;
    }

    void Manager::setStunRelayTcpService(const QString &host, int port, const XMPP::AdvancedConnector::Proxy &proxy,
                                         const QString &user, const QString &pass)
    {
        d->stunRelayTcpHost = host;
        d->stunRelayTcpPort = port;
        d->stunRelayTcpUser = user;
        d->stunRelayTcpPass = pass;

        XMPP::TurnClient::Proxy tproxy;

        if (proxy.type() == XMPP::AdvancedConnector::Proxy::HttpConnect) {
            tproxy.setHttpConnect(proxy.host(), proxy.port());
            tproxy.setUserPass(proxy.user(), proxy.pass());
        } else if (proxy.type() == XMPP::AdvancedConnector::Proxy::Socks) {
            tproxy.setSocks(proxy.host(), proxy.port());
            tproxy.setUserPass(proxy.user(), proxy.pass());
        }

        d->stunProxy = tproxy;
    }

    //----------------------------------------------------------------
    // Pad
    //----------------------------------------------------------------
    Pad::Pad(Manager *manager, Session *session) : _manager(manager), _session(session)
    {
        auto reserver = _session->manager()->client()->tcpPortReserver();
        _discoScope   = reserver->scope(QString::fromLatin1("ice"));
    }

    QString Pad::ns() const
    {
        const auto requested = requestedNamespace();
        return requested.isEmpty() ? NS : requested;
    }

    QSharedPointer<IceConnection> Pad::connectionFor(Transport *transport)
    {
        // Scoped by this session's pad, never by peer JID. Only this pad's
        // transports can acquire a connection here.
        if (!transport || transport->pad().data() != this)
            return {};
        auto it = _connections.find(transport);
        if (it != _connections.end())
            return it.value().toStrongRef();

        auto connection = QSharedPointer<IceConnection>::create();
        _connections.insert(transport, connection.toWeakRef());
        connect(transport, &QObject::destroyed, this, [this, transport]() { _connections.remove(transport); });
        return connection;
    }

    Session *Pad::session() const { return _session; }

    TransportManager *Pad::manager() const { return _manager; }

    void Pad::onLocalAccepted()
    {
        // Session validates signaled group membership, but each Transport still
        // uses an independent ICE connection. Do not advertise BUNDLE until
        // negotiated groups drive shared ICE/DTLS ownership and packet routing.
    }

} // namespace Ice
} // namespace Jingle
} // namespace XMPP

#include "jingle-ice.moc"
