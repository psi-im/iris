from pathlib import Path

# Keep the architecture note aligned with RFC 9143 section 7.5.1.
docs = Path('docs/jingle.md')
s = docs.read_text()
old = '''local rejection, peer rejection/IQ error, content teardown, or failed commit rolls back only the
new provisional membership, leaving existing RTP members and the shared ICE/DTLS association
alive. A responder may also accept the new content independently by answering with the previous
committed grouping rather than the proposed extension.
'''
new = '''local rejection, peer rejection/IQ error, content teardown, or failed commit rolls back only the
new provisional membership, leaving existing RTP members and the shared ICE/DTLS association
alive. Active BUNDLE extension follows RFC 9143 section 7.5.1: accepting the newly added content
also accepts its membership in the proposed BUNDLE. A responder that cannot use the established
shared transport rejects that content; it does not accept the content independently by answering
with the previous committed grouping. Moving an accepted member out of BUNDLE requires a later
negotiation.
'''
if s.count(old) != 1:
    raise SystemExit('stale active-BUNDLE documentation marker not found')
docs.write_text(s.replace(old, new, 1))

# Add an initiator-side signaling regression for the RFC 9143 answer rule.
test = Path('tests/jingle/bundlesignaling.cpp')
s = test.read_text()
marker = 'static void exerciseInitiatorReplacement(const WireOffer &transportSource, TcpPortReserver *reserver)\n'
if s.count(marker) != 1:
    raise SystemExit('initiator replacement marker not found')
block = r'''
#ifdef IRIS_TEST_SCTP
static void exerciseInitiatorRejectsUnbundledContentAccept(const WireOffer &transportSource,
                                                           TcpPortReserver *reserver)
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("rfc9143-answer@example.test/device"));
    auto features = rtpIcePeerFeatures(client, rtp);
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:s5b:1"));
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:ibb:1"));
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:ice-udp:1"));
    setPeerFeatures(client, peer, features);

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto audio = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    auto video = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("video"), J::Origin::Both));
    check(audio && video, "RFC 9143 answer fixture could not create RTP applications");

    RootTaskKeeper pendingInitiate(client.rootTask());
    session.initiate();
    auto audioTransport = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto videoTransport = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    check(audioTransport && videoTransport,
          "RFC 9143 answer fixture did not select initial ICE transports");
    auto icePad = audioTransport->pad().staticCast<J::ICE::Pad>();

    check(waitFor([&]() { return session.state() == J::State::Unacked; }),
          "RFC 9143 answer fixture did not serialize session-initiate");
    acknowledgeJingleTask(pendingInitiate, peer);
    check(session.state() == J::State::Pending,
          "RFC 9143 answer fixture did not reach pending session state");

    QDomDocument initialAcceptDoc;
    auto initialAccept = sessionAcceptPayload(
        initialAcceptDoc, session, transportSource, audio, video, peer);
    check(session.updateFromXml(J::Action::SessionAccept, initialAccept)
              && session.state() == J::State::Active,
          "RFC 9143 answer fixture could not establish active RTP BUNDLE");
    check(session.negotiatedGroupings().size() == 1
              && session.negotiatedGroupings().first().contents
                     == QStringList({ audio->contentName(), video->contentName() })
              && icePad->liveAssociationCount() == 1,
          "RFC 9143 answer fixture did not establish one committed BUNDLE association");

    std::unique_ptr<J::Application> ftOwner(
        session.newContent(J::FileTransfer::NS, J::Origin::Initiator));
    auto ft = dynamic_cast<J::FileTransfer::Application *>(ftOwner.get());
    check(ft, "RFC 9143 answer fixture could not create file-transfer content");
    J::FileTransfer::File file;
    file.setName(QStringLiteral("rfc9143-answer.bin"));
    file.setSize(32);
    file.addHash(Hash::from(Hash::Sha256, QByteArray("rfc9143-active-answer")));
    ft->setFile(file);

    RootTaskKeeper pendingAdd(client.rootTask());
    session.addContent(ftOwner.release());
    auto ftTransport = qSharedPointerDynamicCast<J::ICE::Transport>(ft->transport());
    check(bool(ftTransport), "active outgoing extension did not select ICE for file transfer");
    check(waitFor([&]() { return pendingAdd.task() != nullptr && ft->state() == J::State::Unacked; }),
          "active outgoing extension did not serialize content-add");

    bool ftBound = false, ftRequired = false;
    auto *shared = icePad->groupedConnectionFor(ftTransport.data(), &ftBound, &ftRequired);
    check(shared && ftBound && ftRequired && icePad->liveAssociationCount() == 1,
          "outgoing extension did not stage file transfer on established BUNDLE association");
    acknowledgeJingleTask(pendingAdd, peer);
    check(ft->state() == J::State::Pending,
          "content-add acknowledgement did not leave the new content pending peer answer");

    // RFC 9143 section 7.5.1 forbids accepting a newly added media/content
    // description while removing it from the BUNDLE in that same answer. The
    // peer must reject the content instead if it cannot accept the membership.
    QDomDocument invalidDoc;
    J::Jingle invalidJingle(J::Action::ContentAccept, session.sid());
    auto invalid = invalidJingle.toXml(&invalidDoc);
    invalidDoc.appendChild(invalid);
    J::ContentBase cb(J::Origin::Initiator, ft->contentName());
    cb.senders = J::Origin::Initiator;
    invalid.appendChild(cb.toXml(&invalidDoc, QStringLiteral("content"), J::NS));

    auto oldGroup = invalidDoc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"),
                                               QStringLiteral("group"));
    oldGroup.setAttribute(QStringLiteral("semantics"), QStringLiteral("BUNDLE"));
    for (const auto &name : { audio->contentName(), video->contentName() }) {
        auto member = invalidDoc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"),
                                                 QStringLiteral("content"));
        member.setAttribute(QStringLiteral("name"), name);
        oldGroup.appendChild(member);
    }
    invalid.appendChild(oldGroup);

    check(!session.updateFromXml(J::Action::ContentAccept, invalid),
          "initiator accepted content-accept that removed the new member from BUNDLE");
    check(session.negotiatedGroupings().size() == 1
              && session.negotiatedGroupings().first().contents
                     == QStringList({ audio->contentName(), video->contentName() })
              && icePad->liveAssociationCount() == 1,
          "invalid unbundled content-accept mutated committed topology or association lifetime");

    bool stillBound = false, stillRequired = false;
    check(icePad->groupedConnectionFor(ftTransport.data(), &stillBound, &stillRequired) == nullptr
              && !stillBound && !stillRequired,
          "invalid unbundled content-accept retained provisional BUNDLE membership");
}
#endif

'''
s = s.replace(marker, block + marker, 1)

main_old = '''#ifdef IRIS_TEST_SCTP
    exerciseMixedRtpFileTransferBundle(&reserver);
    exerciseActiveFileTransferBundleExtension(offer, &reserver);
#endif
    exerciseInitiatorReplacement(offer, &reserver);'''
main_new = '''#ifdef IRIS_TEST_SCTP
    exerciseMixedRtpFileTransferBundle(&reserver);
    exerciseActiveFileTransferBundleExtension(offer, &reserver);
    exerciseInitiatorRejectsUnbundledContentAccept(offer, &reserver);
#endif
    exerciseInitiatorReplacement(offer, &reserver);'''
if s.count(main_old) != 1:
    raise SystemExit('main SCTP call marker not found')
s = s.replace(main_old, main_new, 1)
test.write_text(s)
