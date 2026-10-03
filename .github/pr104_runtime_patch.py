from pathlib import Path

ice = Path('src/xmpp/xmpp-im/jingle-ice.cpp')
text = ice.read_text()
old = '''    bool Pad::commitGroupExtension(const ContentKey &content)\n    {\n        if (!d->extensionContent || *d->extensionContent != content)\n            return true;\n'''
new = '''    bool Pad::commitGroupExtension(const ContentKey &content)\n    {\n        // Session calls this only for a negotiated pending extension. Missing\n        // staging is therefore a transaction failure, not an idempotent success:\n        // otherwise negotiatedGroups could publish membership that never joined\n        // the physical association.\n        if (!d->extensionContent || *d->extensionContent != content)\n            return false;\n'''
if old not in text:
    raise SystemExit('commitGroupExtension marker not found')
ice.write_text(text.replace(old, new, 1))

test = Path('tests/jingle/bundlesignaling.cpp')
text = test.read_text()
marker = 'static void exerciseInitiatorReplacement(const WireOffer &transportSource, TcpPortReserver *reserver)\n'
if marker not in text:
    raise SystemExit('bundlesignaling insertion marker not found')
block = r'''
#ifdef IRIS_TEST_SCTP
static QDomElement activeFileAddPayload(QDomDocument &doc, const J::Session &session,
                                        const WireOffer &transportSource, const QString &name,
                                        const QStringList &bundleMembers)
{
    J::Jingle jingle(J::Action::ContentAdd, session.sid());
    auto root = jingle.toXml(&doc);
    doc.appendChild(root);

    J::ContentBase cb(J::Origin::Initiator, name);
    cb.senders = J::Origin::Initiator;
    auto content = cb.toXml(&doc, QStringLiteral("content"), J::NS);

    J::FileTransfer::File file;
    file.setName(name + QStringLiteral(".bin"));
    file.setSize(32);
    auto description = doc.createElementNS(J::FileTransfer::NS, QStringLiteral("description"));
    description.appendChild(file.toXml(&doc));
    content.appendChild(description);

    const auto source = sourceContent(transportSource, transportSource.audioName);
    check(!source.isNull(), "active extension fixture could not find source content");
    const auto sourceTransport = source.firstChildElement(QStringLiteral("transport"));
    check(!sourceTransport.isNull(), "active extension fixture could not find source transport");
    content.appendChild(doc.importNode(sourceTransport, true));
    root.appendChild(content);

    auto group = doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"),
                                     QStringLiteral("group"));
    group.setAttribute(QStringLiteral("semantics"), QStringLiteral("BUNDLE"));
    for (const auto &memberName : bundleMembers) {
        auto member = doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"),
                                          QStringLiteral("content"));
        member.setAttribute(QStringLiteral("name"), memberName);
        group.appendChild(member);
    }
    root.appendChild(group);
    return root;
}

static void rejectJingleTask(RootTaskKeeper &keeper, const Jid &peer)
{
    auto pending = keeper.task();
    check(pending, "content-accept did not create a retained Jingle IQ task");

    QDomDocument replyDoc;
    auto reply = replyDoc.createElement(QStringLiteral("iq"));
    replyDoc.appendChild(reply);
    reply.setAttribute(QStringLiteral("type"), QStringLiteral("error"));
    reply.setAttribute(QStringLiteral("from"), peer.full());
    reply.setAttribute(QStringLiteral("id"), pending->id());
    auto error = replyDoc.createElement(QStringLiteral("error"));
    error.setAttribute(QStringLiteral("type"), QStringLiteral("cancel"));
    error.appendChild(replyDoc.createElementNS(QStringLiteral("urn:ietf:params:xml:ns:xmpp-stanzas"),
                                               QStringLiteral("service-unavailable")));
    reply.appendChild(error);
    check(pending->take(reply), "content-accept IQ error was not consumed");
    keeper.release();
}

static void exerciseActiveFileTransferBundleExtension(const WireOffer &offer, TcpPortReserver *reserver)
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("initiator@example.test/device"));
    auto features = rtpIcePeerFeatures(client, rtp);
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:s5b:1"));
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:ibb:1"));
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:ice-udp:1"));
    setPeerFeatures(client, peer, features);

    J::Session session(client.jingleManager(), peer, J::Origin::Responder);
    J::Jingle parsed(offer.root);
    check(parsed.isValid() && session.incomingInitiate(parsed, offer.root),
          "active extension responder rejected initial BUNDLE");

    auto audio = dynamic_cast<J::RTP::Application *>(session.content(offer.audioName, J::Origin::Initiator));
    auto video = dynamic_cast<J::RTP::Application *>(session.content(offer.videoName, J::Origin::Initiator));
    check(audio && video, "active extension fixture lost initial RTP applications");
    auto audioTransport = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto videoTransport = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    check(audioTransport && videoTransport, "active extension fixture lost initial ICE transports");
    auto icePad = audioTransport->pad().staticCast<J::ICE::Pad>();

    RootTaskKeeper pendingInitialAccept(client.rootTask());
    session.accept();
    check(waitFor([&]() { return pendingInitialAccept.task() != nullptr; }),
          "responder did not serialize initial session-accept");
    acknowledgeJingleTask(pendingInitialAccept, peer);
    check(waitFor([&]() { return session.state() == J::State::Active; }),
          "responder did not become Active after session-accept acknowledgement");
    check(session.negotiatedGroupings().size() == 1
              && session.negotiatedGroupings().first().contents
                     == QStringList({ offer.audioName, offer.videoName })
              && icePad->liveAssociationCount() == 1,
          "active extension fixture did not establish initial BUNDLE");

    bool audioBound = false, audioRequired = false;
    bool videoBound = false, videoRequired = false;
    auto *shared = icePad->groupedConnectionFor(audioTransport.data(), &audioBound, &audioRequired);
    check(shared && audioBound && audioRequired
              && icePad->groupedConnectionFor(videoTransport.data(), &videoBound, &videoRequired) == shared
              && videoBound && videoRequired,
          "initial active RTP contents did not share one association");

    const QString fileName = QStringLiteral("active-file");
    QDomDocument addDoc;
    auto add = activeFileAddPayload(addDoc, session, offer, fileName,
                                    { offer.audioName, offer.videoName, fileName });
    check(session.updateFromXml(J::Action::ContentAdd, add),
          "valid active BUNDLE file-transfer extension was rejected");
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    auto ft = dynamic_cast<J::FileTransfer::Application *>(
        session.content(fileName, J::Origin::Initiator));
    check(ft, "active BUNDLE extension did not create file-transfer application");
    auto ftTransport = qSharedPointerDynamicCast<J::ICE::Transport>(ft->transport());
    check(ftTransport, "active BUNDLE extension did not create ICE file-transfer transport");
    check(session.negotiatedGroupings().first().contents
              == QStringList({ offer.audioName, offer.videoName }),
          "incoming content-add published BUNDLE membership before content-accept");

    RootTaskKeeper pendingContentAccept(client.rootTask());
    ft->prepare();
    check(waitFor([&]() { return pendingContentAccept.task() != nullptr; }),
          "file-transfer extension did not serialize content-accept");

    bool ftBound = false, ftRequired = false;
    auto *provisional = icePad->groupedConnectionFor(ftTransport.data(), &ftBound, &ftRequired);
    check(ftBound && ftRequired && provisional == shared && icePad->liveAssociationCount() == 1,
          "provisional file-transfer extension did not reuse the established association");
    check(session.negotiatedGroupings().first().contents
              == QStringList({ offer.audioName, offer.videoName }),
          "provisional physical binding leaked into negotiated topology");

    acknowledgeJingleTask(pendingContentAccept, peer);
    check(waitFor([&]() {
              const auto groups = session.negotiatedGroupings();
              return groups.size() == 1 && groups.first().contents
                  == QStringList({ offer.audioName, offer.videoName, fileName });
          }),
          "content-accept acknowledgement did not commit active BUNDLE extension");
    check(icePad->liveAssociationCount() == 1,
          "committed active BUNDLE extension allocated another association");

    bool committedBound = false, committedRequired = false;
    check(icePad->groupedConnectionFor(ftTransport.data(), &committedBound, &committedRequired) == shared
              && committedBound && committedRequired,
          "committed file-transfer content did not remain on established association");

    const QString rejectedName = QStringLiteral("rejected-file");
    QDomDocument rejectedDoc;
    auto rejectedAdd = activeFileAddPayload(
        rejectedDoc, session, offer, rejectedName,
        { offer.audioName, offer.videoName, fileName, rejectedName });
    check(session.updateFromXml(J::Action::ContentAdd, rejectedAdd),
          "second active BUNDLE extension was rejected before rollback test");
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    auto rejectedFt = dynamic_cast<J::FileTransfer::Application *>(
        session.content(rejectedName, J::Origin::Initiator));
    check(rejectedFt, "rollback fixture did not create second file-transfer application");

    RootTaskKeeper rejectedAccept(client.rootTask());
    rejectedFt->prepare();
    check(waitFor([&]() { return rejectedAccept.task() != nullptr; }),
          "rollback fixture did not serialize content-accept");
    rejectJingleTask(rejectedAccept, peer);
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    check(session.negotiatedGroupings().size() == 1
              && session.negotiatedGroupings().first().contents
                     == QStringList({ offer.audioName, offer.videoName, fileName })
              && icePad->liveAssociationCount() == 1,
          "failed content-accept changed established BUNDLE topology or lifetime");
    check(shared && audioTransport->state() < J::State::Finishing
              && videoTransport->state() < J::State::Finishing,
          "extension rollback disturbed established RTP members");

    const QString malformedName = QStringLiteral("malformed-file");
    QDomDocument malformedDoc;
    auto malformed = activeFileAddPayload(
        malformedDoc, session, offer, malformedName,
        { offer.videoName, offer.audioName, fileName, malformedName });
    check(!session.updateFromXml(J::Action::ContentAdd, malformed),
          "active BUNDLE extension accepted established-member reordering");
    check(!session.content(malformedName, J::Origin::Initiator)
              && icePad->liveAssociationCount() == 1,
          "malformed active BUNDLE extension mutated runtime state");
}
#endif

'''
test.write_text(text.replace(marker, block + marker, 1))

main_old = '''#ifdef IRIS_TEST_SCTP
    exerciseMixedRtpFileTransferBundle(&reserver);
#endif
    exerciseInitiatorReplacement(offer, &reserver);'''
main_new = '''#ifdef IRIS_TEST_SCTP
    exerciseMixedRtpFileTransferBundle(&reserver);
    exerciseActiveFileTransferBundleExtension(offer, &reserver);
#endif
    exerciseInitiatorReplacement(offer, &reserver);'''
text = test.read_text()
if main_old not in text:
    raise SystemExit('bundlesignaling main marker not found')
test.write_text(text.replace(main_old, main_new, 1))
