// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/jingle-ice-connection_p.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <qca.h>

#define private public
#include <iris/jingle-session.h>
#include <iris/jingle-ice.h>
#undef private

#include <iris/jingle-rtp.h>
#include <iris/xmpp_caps.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_task.h>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class Ack final : public Task {
public:
    explicit Ack(Task *parent) : Task(parent) { setSuccess(); }
};

class Endpoint final : public J::RTP::MediaEndpoint {
public:
    explicit Endpoint(QString media) : media_(std::move(media)) { }

    J::RTP::Description localOffer() const override
    {
        J::RTP::Description description;
        description.media   = media_;
        description.rtcpMux = true;
        description.ssrc    = media_ == QLatin1String("audio") ? 0x11111111u : 0x22222222u;
        J::RTP::PayloadType payload;
        payload.id        = media_ == QLatin1String("audio") ? 96 : 97;
        payload.name      = media_ == QLatin1String("audio") ? QStringLiteral("opus") : QStringLiteral("VP8");
        payload.clockrate = media_ == QLatin1String("audio") ? 48000u : 90000u;
        if (media_ == QLatin1String("audio"))
            payload.channels = 2;
        description.payloads.append(payload);
        return description;
    }

    std::optional<J::RTP::Description> makeAnswer(const J::RTP::Description &offer) const override
    {
        if (offer.media != media_ || !offer.rtcpMux || offer.payloads.isEmpty())
            return {};
        auto answer = offer;
        // Distinguish responder SSRCs while preserving offered PT identifiers.
        answer.ssrc = media_ == QLatin1String("audio") ? 0x33333333u : 0x44444444u;
        return answer;
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
    QString      media_;
    PacketWriter writer_;
};

class MediaSession final : public J::RTP::MediaSession {
public:
    std::unique_ptr<J::RTP::MediaEndpoint> createEndpoint(const QString &, const QString &media) override
    {
        if (media != QLatin1String("audio") && media != QLatin1String("video"))
            return {};
        return std::make_unique<Endpoint>(media);
    }
};

class Provider final : public J::RTP::MediaProvider {
public:
    std::unique_ptr<J::RTP::MediaSession> createSession() override { return std::make_unique<MediaSession>(); }
    QStringList mediaTypes() const override { return { QStringLiteral("audio"), QStringLiteral("video") }; }
};

static void setPeerFeatures(Client &client, const Jid &peer, QStringList features)
{
    features.removeDuplicates();
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:jingle-bundle-signaling"));
    disco.setFeatures(Features(features));

    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

static QStringList rtpIcePeerFeatures(Client &client, J::RTP::Manager *rtp)
{
    const auto features = client.jingleManager()->discoFeatures();
    check(features.contains(J::RTP::Description::ns()), "production caps omitted RTP description support");
    check(features.contains(J::ICE::NS), "production caps omitted ICE support");
    check(features.contains(QStringLiteral("urn:ietf:rfc:5888")),
          "production caps omitted grouping support on the feature branch");
    Q_UNUSED(rtp);
    return features;
}

static bool waitFor(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return condition();
}

struct WireOffer {
    QDomDocument doc;
    QDomElement  root;
    QString      audioName;
    QString      videoName;
};

static WireOffer makeOffer(Client &client, TcpPortReserver *reserver)
{
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("responder@example.test/device"));
    setPeerFeatures(client, peer, rtpIcePeerFeatures(client, rtp));

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto audio = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    auto video = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("video"), J::Origin::Both));
    check(audio && video, "failed to create outgoing RTP applications");

    const QString audioName = audio->contentName();
    const QString videoName = video->contentName();
    check(session.setGroupings(
              { J::ContentGroup { QStringLiteral("BUNDLE"), { audioName, videoName } } }),
          "initiator BUNDLE proposal rejected");

    audio->prepare();
    video->prepare();

    auto audioTransport = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto videoTransport = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    check(audioTransport && videoTransport, "caps-driven RTP selection did not choose ICE");
    auto icePad = audioTransport->pad().staticCast<J::ICE::Pad>();

    check(waitFor([&]() {
              return audio->localDescription() && video->localDescription() && audioTransport->hasUpdates()
                  && videoTransport->hasUpdates();
          }),
          "outgoing BUNDLE offer did not finish RTP/ICE preparation");
    check(icePad->liveAssociationCount() == 1, "initiator BUNDLE offer did not stage one association");

    auto [audioTransportXml, audioAck] = audioTransport->takeOutgoingUpdate(false);
    auto [videoTransportXml, videoAck] = videoTransport->takeOutgoingUpdate(false);
    check(!audioTransportXml.isNull() && !videoTransportXml.isNull(), "missing BUNDLE ICE offer updates");

    // Complete the local IQ boundary so fingerprint state is not left half-sent
    // while this temporary initiator Session is destroyed.
    if (audioAck) {
        Ack result(client.rootTask());
        audioAck(&result);
    }
    if (videoAck) {
        Ack result(client.rootTask());
        videoAck(&result);
    }

    WireOffer offer;
    offer.audioName = audioName;
    offer.videoName = videoName;

    J::Jingle jingle(J::Action::SessionInitiate, QStringLiteral("bundle-signaling-test"));
    jingle.setInitiator(Jid(QStringLiteral("initiator@example.test/device")));
    offer.root = jingle.toXml(&offer.doc);

    auto appendContent = [&](J::RTP::Application *application, const QDomElement &transportXml) {
        J::ContentBase content(J::Origin::Initiator, application->contentName());
        content.senders = J::Origin::Both;
        auto contentXml = content.toXml(&offer.doc, QStringLiteral("content"),
                                        QStringLiteral("urn:xmpp:jingle:1"));
        check(contentXml.namespaceURI() == QLatin1String("urn:xmpp:jingle:1"),
              "wire offer content lost the Jingle namespace");
        contentXml.appendChild(offer.doc.importNode(application->makeLocalOffer(), true));
        contentXml.appendChild(offer.doc.importNode(transportXml, true));
        offer.root.appendChild(contentXml);
    };
    appendContent(audio, audioTransportXml);
    appendContent(video, videoTransportXml);

    auto group = offer.doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"),
                                           QStringLiteral("group"));
    group.setAttribute(QStringLiteral("semantics"), QStringLiteral("BUNDLE"));
    for (const auto &name : { audioName, videoName }) {
        auto member = offer.doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"),
                                                QStringLiteral("content"));
        member.setAttribute(QStringLiteral("name"), name);
        group.appendChild(member);
    }
    offer.root.appendChild(group);

    return offer;
}

static QDomElement sourceContent(const WireOffer &offer, const QString &name)
{
    for (auto content = offer.root.firstChildElement(QStringLiteral("content")); !content.isNull();
         content = content.nextSiblingElement(QStringLiteral("content"))) {
        if (content.attribute(QStringLiteral("name")) == name)
            return content;
    }
    return {};
}

static QDomElement replacementPayload(
    QDomDocument &doc, const WireOffer &offer, const QList<QPair<QString, QString>> &contents)
{
    auto root = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    doc.appendChild(root);

    for (const auto &[name, sourceName] : contents) {
        const auto source = sourceContent(offer, sourceName);
        check(!source.isNull(), "replacement fixture could not find offered content");
        auto sourceTransport = source.firstChildElement(QStringLiteral("transport"));
        check(!sourceTransport.isNull(), "replacement fixture could not find offered ICE transport");

        J::ContentBase cb(J::Origin::Initiator, name);
        cb.senders = J::Origin::Both;
        auto content = cb.toXml(&doc, QStringLiteral("content"), J::NS);
        auto transport = doc.importNode(sourceTransport, true).toElement();
        transport.setAttribute(QStringLiteral("ufrag"), QStringLiteral("bundle-restart-ufrag"));
        transport.setAttribute(QStringLiteral("pwd"), QStringLiteral("bundle-restart-password"));
        content.appendChild(transport);
        root.appendChild(content);
    }
    return root;
}

static QSet<Task *> directRootTasks(Client &client)
{
    QSet<Task *> tasks;
    for (auto child : client.rootTask()->children()) {
        if (auto task = qobject_cast<Task *>(child))
            tasks.insert(task);
    }
    return tasks;
}

static void acknowledgeNewJingleTask(Client &client, const Jid &peer, const QSet<Task *> &before)
{
    Task *pending = nullptr;
    for (auto child : client.rootTask()->children()) {
        auto task = qobject_cast<Task *>(child);
        if (!task || before.contains(task))
            continue;
        check(!pending, "more than one root task appeared while sending session-initiate");
        pending = task;
    }
    check(pending, "session-initiate did not create a Jingle IQ task");

    QDomDocument replyDoc;
    auto reply = replyDoc.createElement(QStringLiteral("iq"));
    replyDoc.appendChild(reply);
    reply.setAttribute(QStringLiteral("type"), QStringLiteral("result"));
    reply.setAttribute(QStringLiteral("from"), peer.full());
    reply.setAttribute(QStringLiteral("id"), pending->id());
    check(pending->take(reply), "session-initiate IQ result was not consumed");
}

static QDomElement sessionAcceptPayload(
    QDomDocument &doc, const J::Session &session, const WireOffer &transportSource,
    J::RTP::Application *audio, J::RTP::Application *video, const Jid &peer)
{
    J::Jingle jingle(J::Action::SessionAccept, session.sid());
    jingle.setResponder(peer);
    auto root = jingle.toXml(&doc);
    doc.appendChild(root);

    auto appendAnswer = [&](J::RTP::Application *application, const QString &sourceName, quint32 ssrc) {
        const auto local = application->localDescription();
        check(local.has_value(), "initiator RTP offer disappeared before session-accept");

        auto answer = *local;
        answer.ssrc = ssrc;

        const auto source = sourceContent(transportSource, sourceName);
        check(!source.isNull(), "session-accept fixture could not find source ICE content");
        auto sourceTransport = source.firstChildElement(QStringLiteral("transport"));
        check(!sourceTransport.isNull(), "session-accept fixture could not find source ICE transport");

        J::ContentBase cb(J::Origin::Initiator, application->contentName());
        cb.senders = J::Origin::Both;
        auto content = cb.toXml(&doc, QStringLiteral("content"), J::NS);
        content.appendChild(answer.toXml(doc));

        auto transport = doc.importNode(sourceTransport, true).toElement();
        transport.setAttribute(QStringLiteral("ufrag"), QStringLiteral("bundle-answer-ufrag"));
        transport.setAttribute(QStringLiteral("pwd"), QStringLiteral("bundle-answer-password"));
        auto fingerprint = transport.firstChildElement(QStringLiteral("fingerprint"));
        if (!fingerprint.isNull())
            fingerprint.setAttribute(QStringLiteral("setup"), QStringLiteral("passive"));
        content.appendChild(transport);
        root.appendChild(content);
    };

    appendAnswer(audio, transportSource.audioName, 0x33333333u);
    appendAnswer(video, transportSource.videoName, 0x44444444u);

    auto group = doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"),
                                     QStringLiteral("group"));
    group.setAttribute(QStringLiteral("semantics"), QStringLiteral("BUNDLE"));
    for (auto application : { audio, video }) {
        auto member = doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"),
                                          QStringLiteral("content"));
        member.setAttribute(QStringLiteral("name"), application->contentName());
        group.appendChild(member);
    }
    root.appendChild(group);
    return root;
}

static void exerciseResponder(const WireOffer &offer, TcpPortReserver *reserver, bool acceptBundle)
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("initiator@example.test/device"));
    setPeerFeatures(client, peer, rtpIcePeerFeatures(client, rtp));

    J::Session session(client.jingleManager(), peer, J::Origin::Responder);
    J::Jingle parsed(offer.root);
    check(parsed.isValid() && parsed.action() == J::Action::SessionInitiate, "wire BUNDLE offer did not parse");
    check(session.incomingInitiate(parsed, offer.root), "responder rejected BUNDLE session-initiate");

    check(session.remoteGroupings().size() == 1
              && session.remoteGroupings().first().semantics == QLatin1String("BUNDLE")
              && session.remoteGroupings().first().contents
                     == QStringList({ offer.audioName, offer.videoName }),
          "responder lost offered BUNDLE membership");

    auto audio = dynamic_cast<J::RTP::Application *>(
        session.content(offer.audioName, J::Origin::Initiator));
    auto video = dynamic_cast<J::RTP::Application *>(
        session.content(offer.videoName, J::Origin::Initiator));
    check(audio && video, "responder did not create production RTP applications");

    auto audioTransport = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto videoTransport = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    check(audioTransport && videoTransport, "responder did not create production ICE transports");
    auto icePad = audioTransport->pad().staticCast<J::ICE::Pad>();

    // Incoming transport updates are committed asynchronously. Before local
    // consent they must remain pure per-content signaling state and allocate no
    // physical association.
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    check(icePad->liveAssociationCount() == 0,
          "responder allocated BUNDLE association before local grouping decision");

    if (acceptBundle) {
        check(session.setGroupings(
                  { J::ContentGroup { QStringLiteral("BUNDLE"), { offer.audioName, offer.videoName } } }),
              "responder could not accept offered BUNDLE group");
    } else {
        check(session.setGroupings({}), "responder could not refuse BUNDLE");
    }

    session.accept();

    const qsizetype expectedAssociations = acceptBundle ? 1 : 2;
    check(waitFor([&]() {
              return audio->state() >= J::State::ApprovedToSend && video->state() >= J::State::ApprovedToSend
                  && icePad->liveAssociationCount() == expectedAssociations;
          }),
          acceptBundle ? "accepted BUNDLE did not allocate one shared association"
                       : "BUNDLE refusal did not allocate independent associations");

    bool audioBound = false, audioRequired = false, videoBound = false, videoRequired = false;
    auto *audioNetwork = icePad->groupedConnectionFor(audioTransport.data(), &audioBound, &audioRequired);
    auto *videoNetwork = icePad->groupedConnectionFor(videoTransport.data(), &videoBound, &videoRequired);
    check(audioBound && videoBound, "responder transports lost content identity");

    if (acceptBundle) {
        check(audioRequired && videoRequired && audioNetwork && audioNetwork == videoNetwork,
              "accepted BUNDLE did not bind both contents to one staged association");
        check(audioTransport->rtpSession() && audioTransport->rtpSession() == videoTransport->rtpSession(),
              "accepted BUNDLE did not share responder SRTP");

    } else {
        check(!audioRequired && !videoRequired && !audioNetwork && !videoNetwork,
              "BUNDLE refusal retained staged shared membership");
        check(audioTransport->rtpSession() && videoTransport->rtpSession()
                  && audioTransport->rtpSession() != videoTransport->rtpSession(),
              "BUNDLE refusal did not retain independent SRTP associations");
    }
}

static void exerciseInitiatorReplacement(const WireOffer &transportSource, TcpPortReserver *reserver)
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("responder@example.test/device"));
    setPeerFeatures(client, peer, rtpIcePeerFeatures(client, rtp));

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto audio = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    auto video = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("video"), J::Origin::Both));
    check(audio && video, "replacement initiator could not create RTP applications");
    check(session.setGroupings(
              { J::ContentGroup { QStringLiteral("BUNDLE"), { audio->contentName(), video->contentName() } } }),
          "replacement initiator could not offer BUNDLE");

    // Exercise the real outgoing session-initiate state machine. initiate()
    // owns the production preparation boundary: RTP selects ICE synchronously
    // from prepare(), while media/DTLS completion and stanza serialization are
    // asynchronous. Do not inspect transport selection before this call.
    const auto tasksBeforeInitiate = directRootTasks(client);
    session.initiate();

    auto audioTransport = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto videoTransport = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    check(audioTransport && videoTransport, "replacement initiator did not select ICE during initiate()");
    auto icePad = audioTransport->pad().staticCast<J::ICE::Pad>();

    // There is no connected XMPP stream in this test process, so Task::go()
    // intentionally stops at the wire boundary; inject the peer's real IQ result
    // through the existing Task parser to complete exactly that transaction.
    check(waitFor([&]() { return session.state() == J::State::Unacked; }),
          "replacement initiator did not serialize session-initiate");
    acknowledgeNewJingleTask(client, peer, tasksBeforeInitiate);
    check(session.state() == J::State::Pending
              && audio->state() == J::State::Pending
              && video->state() == J::State::Pending,
          "session-initiate IQ result did not establish the pending negotiation boundary");
    check(icePad->liveAssociationCount() == 1,
          "outgoing negotiated BUNDLE offer did not retain one shared association");

    // Feed a genuine session-accept document through the same production parser
    // used by JTPush. The payload uses production RTP description serialization
    // and a production-generated ICE transport snapshot.
    QDomDocument acceptDoc;
    auto accept = sessionAcceptPayload(acceptDoc, session, transportSource, audio, video, peer);
    check(session.updateFromXml(J::Action::SessionAccept, accept),
          "production session-accept XML was rejected");
    check(session.state() == J::State::Active
              && audio->state() == J::State::Accepted
              && video->state() == J::State::Accepted,
          "session-accept XML did not establish the active negotiated session");

    bool audioBound = false, audioRequired = false, videoBound = false, videoRequired = false;
    auto *audioNetwork = icePad->groupedConnectionFor(audioTransport.data(), &audioBound, &audioRequired);
    auto *videoNetwork = icePad->groupedConnectionFor(videoTransport.data(), &videoBound, &videoRequired);
    check(audioBound && videoBound && audioRequired && videoRequired && audioNetwork
              && audioNetwork == videoNetwork && icePad->liveAssociationCount() == 1,
          "session-accept did not preserve the original shared BUNDLE association");
    QPointer<J::ICE::IceConnection> oldNetwork(audioNetwork);

    QDomDocument partialDoc;
    auto partial = replacementPayload(
        partialDoc, transportSource,
        { qMakePair(audio->contentName(), transportSource.audioName) });
    check(!session.updateFromXml(J::Action::TransportReplace, partial),
          "partial negotiated BUNDLE transport-replace was accepted");
    check(audio->transport() == audioTransport && video->transport() == videoTransport
              && oldNetwork && icePad->liveAssociationCount() == 1,
          "partial BUNDLE transport-replace mutated the live association");

    QDomDocument fullDoc;
    auto full = replacementPayload(
        fullDoc, transportSource,
        { qMakePair(audio->contentName(), transportSource.audioName),
          qMakePair(video->contentName(), transportSource.videoName) });
    check(session.updateFromXml(J::Action::TransportReplace, full),
          "full negotiated BUNDLE transport-replace was rejected");

    auto replacementAudio = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto replacementVideo = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    check(replacementAudio && replacementVideo && replacementAudio != audioTransport
              && replacementVideo != videoTransport,
          "full BUNDLE transport-replace did not install fresh ICE transports");

    bool replacementAudioBound = false, replacementAudioRequired = false;
    bool replacementVideoBound = false, replacementVideoRequired = false;
    auto *newAudioNetwork = icePad->groupedConnectionFor(
        replacementAudio.data(), &replacementAudioBound, &replacementAudioRequired);
    check(replacementAudioBound && replacementAudioRequired && newAudioNetwork
              && newAudioNetwork != oldNetwork && oldNetwork
              && icePad->liveAssociationCount() == 1,
          "first BUNDLE replacement member did not stage make-before-break");

    QPointer<J::ICE::IceConnection> newNetwork(newAudioNetwork);
    auto *newVideoNetwork = icePad->groupedConnectionFor(
        replacementVideo.data(), &replacementVideoBound, &replacementVideoRequired);
    check(replacementVideoBound && replacementVideoRequired && newVideoNetwork == newNetwork
              && newNetwork && !oldNetwork && icePad->liveAssociationCount() == 1,
          "full BUNDLE replacement did not atomically switch one shared association");

    check(replacementAudio->enableRtpMux() && replacementVideo->enableRtpMux(),
          "replacement BUNDLE transports did not retain RTP-mux compatibility");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    TcpPortReserver  reserver;

    Client initiator;
    const auto offer = makeOffer(initiator, &reserver);
    exerciseResponder(offer, &reserver, true);
    exerciseResponder(offer, &reserver, false);
    exerciseInitiatorReplacement(offer, &reserver);

    qInfo("BUNDLE signaling-to-runtime regressions passed");
    return 0;
}
