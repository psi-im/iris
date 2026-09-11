#include "../../src/xmpp/xmpp-im/jingle-ice-connection_p.h"
#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QtCrypto>
#include <iris/dtls.h>
#include <iris/jingle-session.h>
#include <iris/jingle-transport.h>
#include <iris/xmpp_client.h>

#include <type_traits>

// Inspect the internal per-session registry without publishing a BUNDLE API.
#define private public
#include <iris/jingle-ice.h>
#undef private

using namespace XMPP;
using namespace XMPP::Jingle::ICE;

static_assert(!std::is_copy_constructible_v<ConnectionMembership>);
static_assert(!std::is_copy_assignable_v<ConnectionMembership>);
static_assert(std::is_move_constructible_v<ConnectionMembership>);
static_assert(std::is_move_assignable_v<ConnectionMembership>);

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

int main(int argc, char **argv)
{
    QCoreApplication        app(argc, argv);
    QCA::Initializer        qca;
    auto                    audio       = QSharedPointer<IceConnection>::create();
    auto                    video       = audio;
    auto                    independent = QSharedPointer<IceConnection>::create();
    QPointer<IceConnection> group(audio.data());
    QPointer<IceConnection> other(independent.data());
    check(!group->parent(), "shared connection has a competing QObject owner");

    Component component;
    component.dtls = new Dtls(group, QStringLiteral("local"), QStringLiteral("peer"));
    component.srtp = new Jingle::RTP::SrtpSession(component.dtls, group);
    QPointer<Jingle::RTP::SrtpSession> srtp(component.srtp);
    group->components.append(component);
    QPointer<Dtls> dtls(component.dtls);
    audio.reset();
    check(group && dtls, "releasing one membership destroyed shared resources");
    check(other && other != group, "independent connection was affected");
    video.reset();
    check(!group && !dtls && !srtp, "last membership retained connection or DTLS/SRTP resources");
    check(other, "group teardown destroyed an independent connection");
    independent.reset();
    check(!other, "independent connection leaked");

    auto association = QSharedPointer<IceConnection>::create();
    association->generation = ConnectionGeneration { 7, 11, 13 };
    QPointer<IceConnection> associationGuard(association.data());
    ConnectionMembership audioMembership(association, 42,
                                         Jingle::ContentKey { QStringLiteral("audio"), Jingle::Origin::Initiator });
    ConnectionMembership videoMembership(association, 42,
                                         Jingle::ContentKey { QStringLiteral("video"), Jingle::Origin::Initiator });
    const auto callbackGeneration = audioMembership.generation();
    check(audioMembership.associationId() == 42
              && audioMembership.content()
                  == Jingle::ContentKey { QStringLiteral("audio"), Jingle::Origin::Initiator },
          "membership identity was not retained");
    association.reset();
    audioMembership.reset();
    check(associationGuard, "releasing one explicit membership destroyed the shared association");
    check(videoMembership.generation() == callbackGeneration, "membership generation snapshot changed unexpectedly");
    ++videoMembership.connection()->generation.membershipRevision;
    check(videoMembership.generation() != callbackGeneration, "membership revision did not invalidate a stale token");
    ConnectionMembership movedMembership(std::move(videoMembership));
    check(!videoMembership && movedMembership, "moving membership duplicated or lost its strong share");
    movedMembership.reset();
    check(!associationGuard, "last explicit membership did not release the shared association");

    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);
    Jingle::Session sessionA(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")));
    Jingle::Session sessionB(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")));

    auto builtinIce = client.jingleICEManager();
    check(builtinIce && builtinIce->ns().contains(NS) && builtinIce->ns().contains(NS_ICE_UDP),
          "built-in ICE manager did not register both wire profiles");
    auto modernProfile = sessionA.newOutgoingTransport(NS);
    auto udpProfile    = sessionA.newOutgoingTransport(NS_ICE_UDP);
    check(modernProfile && modernProfile->pad()->ns() == NS, "ice:0 profile lost its namespace");
    check(udpProfile && udpProfile->pad()->ns() == NS_ICE_UDP, "ice-udp:1 profile lost its namespace");
    check(!sessionA.newOutgoingTransport(QStringLiteral("urn:example:unsupported")),
          "unsupported transport namespace created a transport");

    Manager manager;
    auto    padA            = Pad::Ptr::create(&manager, &sessionA);
    auto    padB            = Pad::Ptr::create(&manager, &sessionB);
    auto    first           = QSharedPointer<Transport>::create(padA, Jingle::Origin::Initiator);
    auto    sibling         = QSharedPointer<Transport>::create(padA, Jingle::Origin::Initiator);
    auto    separate        = QSharedPointer<Transport>::create(padB, Jingle::Origin::Initiator);
    auto    firstNetwork    = padA->connectionFor(first.data());
    auto    siblingNetwork  = padA->connectionFor(sibling.data());
    auto    separateNetwork = padB->connectionFor(separate.data());
    check(firstNetwork == padA->connectionFor(first.data()), "registry lost transport identity");
    check(firstNetwork != siblingNetwork, "unbundled contents shared a connection");
    check(firstNetwork != separateNetwork, "sessions to the same peer shared a connection");
    check(!padA->connectionFor(separate.data()), "registry accepted another session's transport");
    check(!padA->connectionFor(nullptr), "registry accepted a null transport");
    if (!Jingle::RTP::SrtpContext::supportedProfiles().isEmpty() && !Dtls::supportedSRTPProfiles().isEmpty()) {
        auto media = QSharedPointer<Transport>::create(padB, Jingle::Origin::Initiator);
        check(media->enableRtpMux(), "explicit secure RTP mode rejected");
        check(!media->rtpSession(), "SRTP binding created before DTLS configuration");
        check(!media->sendRtpPacket({}, Jingle::RTP::SrtpContext::Packet::Rtp, 0), "unprepared media sent");
        check(!media->addChannel(Jingle::TransportFeature::MessageOriented, "raw", 0),
              "secure RTP exposed raw channel");
        media->setComponentsCount(2);
        check(padB->connectionFor(media.data())->components.size() == 1, "mux mode acquired a second component");
        media->stop();
        check(!media->enableRtpMux(), "stopped transport reconfigured");
        auto insecure = QSharedPointer<Transport>::create(padB, Jingle::Origin::Responder);
        check(insecure->enableRtpMux(), "incoming secure RTP mode rejected");
        insecure->prepare();
        check(insecure->state() >= Jingle::State::Finishing, "incoming RTP without fingerprint accepted");
        check(!padB->connectionFor(insecure.data())->ice, "insecure offer started ICE negotiation");
    }
    QPointer<IceConnection> released(firstNetwork.data());
    firstNetwork.reset();
    first.reset();
    check(!released, "pad retained a dead transport's connection");
    check(padA->_connections.size() == 1, "pad retained a dead membership");
    check(padA->connectionFor(sibling.data()) == siblingNetwork, "removal changed sibling's connection");
    qInfo("ICE resource ownership regressions passed");
}
