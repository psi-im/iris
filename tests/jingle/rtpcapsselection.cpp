// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>

#include <iris/xmpp-im/jingle-ibb.h>
#include <iris/jingle-ice.h>
#include <iris/jingle-rtp.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_caps.h>
#include <iris/xmpp_client.h>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class Endpoint final : public J::RTP::MediaEndpoint {
public:
    J::RTP::Description localOffer() const override
    {
        J::RTP::Description description;
        description.media   = QStringLiteral("audio");
        description.rtcpMux = true;
        J::RTP::PayloadType payload;
        payload.id        = 96;
        payload.name      = QStringLiteral("opus");
        payload.clockrate = 48000;
        payload.channels  = 2;
        description.payloads.append(payload);
        return description;
    }

    std::optional<J::RTP::Description> makeAnswer(const J::RTP::Description &offer) const override
    {
        return offer.media == QLatin1String("audio") && offer.rtcpMux ? std::optional(offer) : std::nullopt;
    }

    bool acceptsAnswer(const J::RTP::Description &, const J::RTP::Description &) const override { return true; }
    bool configure(const J::RTP::Description &, const J::RTP::Description &) override { return true; }
    bool supportsPacketIo() const override { return true; }
    bool attachPacketIo(PacketWriter writer) override
    {
        writer_ = std::move(writer);
        return true;
    }
    void receivePacket(const QByteArray &, J::RTP::SrtpContext::Packet) override { }
    void stop() override { writer_ = {}; }

private:
    PacketWriter writer_;
};

class MediaSession final : public J::RTP::MediaSession {
public:
    std::unique_ptr<J::RTP::MediaEndpoint> createEndpoint(const QString &, const QString &media) override
    {
        return media == QLatin1String("audio") ? std::make_unique<Endpoint>() : nullptr;
    }
};

class Provider final : public J::RTP::MediaProvider {
public:
    std::unique_ptr<J::RTP::MediaSession> createSession() override { return std::make_unique<MediaSession>(); }
    QStringList mediaTypes() const override { return { QStringLiteral("audio") }; }
};

static void setPeerFeatures(Client &client, const Jid &peer, QStringList features)
{
    features.removeDuplicates();
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:rtp-caps-selection"));
    disco.setFeatures(Features(features));

    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

static QStringList rtpFeatures(J::RTP::Manager *rtp)
{
    const auto features = rtp->discoFeatures();
    check(features.contains(J::RTP::Description::ns()), "RTP description capability was not advertised");
    check(features.contains(QStringLiteral("urn:xmpp:jingle:apps:rtp:audio")),
          "audio RTP capability was not advertised");
    return features;
}

static void validIceSelection()
{
    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);

    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("ice-peer@example.test/device"));
    QStringList caps = rtpFeatures(rtp);
    caps += client.jingleICEManager()->discoFeatures();
    check(client.jingleManager()->discoFeatures().contains(QStringLiteral("urn:ietf:rfc:5888")),
          "feature branch did not advertise grouping capability");
    setPeerFeatures(client, peer, caps);

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto app = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    check(app, "failed to create RTP application for ICE caps");
    check(app->selectNextTransport(), "RTP application rejected advertised ICE transport");
    check(app->transport() && app->transport()->pad()->ns() == J::ICE::NS,
          "RTP application selected the wrong transport for ICE caps");
}

static void incompatibleTransportCaps(const QStringList &extraCaps, const char *message)
{
    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);

    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    // Current production RTP backend supports packet-oriented ICE only. Keep
    // the policy explicit rather than teaching the generic selector that IBB
    // can carry RTP merely because a future codec might have a tiny bitrate.
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("non-ice-peer@example.test/device"));
    QStringList caps = rtpFeatures(rtp);
    caps += extraCaps;
    setPeerFeatures(client, peer, caps);

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto app = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    check(app, "failed to create RTP application for incompatible caps");
    check(!app->selectNextTransport(), message);
    check(!app->transport(), "RTP application installed an incompatible transport");
    check(app->state() == J::State::Finished,
          "RTP application did not fail closed after exhausting peer transport capabilities");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    {
        Client bare;
        check(bare.jingleManager()->rtpManager()->discoFeatures().isEmpty(),
              "RTP was advertised without a media provider");
    }

    validIceSelection();

    {
        Client probe;
        incompatibleTransportCaps(probe.jingleIBBManager()->discoFeatures(),
                                  "RTP application accepted an IBB-only peer");
    }

    incompatibleTransportCaps({}, "RTP application accepted a peer with no compatible transport capability");

    qInfo("RTP capability/transport selection regressions passed");
    return 0;
}
