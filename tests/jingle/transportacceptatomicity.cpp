// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Reuse the transport-replace fixture to verify that an invalid member of a
// transport-accept batch cannot partially commit an earlier valid member.
#define main iris_transportreplace_legacy_main
#include "transportreplace.cpp"
#undef main

static void testInvalidLaterAcceptIsAtomic(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("accept-atomic@example.test/device")),
                       J::Origin::Initiator);

    auto audio    = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("audio-local"));
    auto audioApp = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audio,
                                   std::make_unique<TestSelector>());
    audioApp->markReplaceInProgress();

    auto video    = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("video-local"));
    auto videoApp = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, video,
                                   std::make_unique<TestSelector>());
    // The transport itself still looks Pending, but this replacement has not
    // been sent/acknowledged and therefore cannot be accepted by the peer.
    videoApp->markReplacePlanned();

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportAccept,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("audio-accepted") },
                        { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("video-out-of-order") } }));

    check(!ok, "transport-accept batch with an out-of-order member was accepted");
    check(audioApp->transport().data() == audio.data(),
          "invalid later transport-accept changed the earlier member's transport identity");
    check(audioApp->replaceInProgress(),
          "invalid later transport-accept partially completed an earlier replacement transaction");
    check(audio->starts() == 0, "invalid later transport-accept partially started an earlier replacement transport");
    check(videoApp->transport().data() == video.data() && videoApp->replacePlanned(),
          "invalid transport-accept member changed its application state");
}

int main(int argc, char **argv)
{
    QCoreApplication     application(argc, argv);
    QCA::Initializer     qca;
    TestTransportManager transportManager;
    Client               client;
    client.jingleManager()->registerTransport(&transportManager);

    testInvalidLaterAcceptIsAtomic(client);

    qInfo("Transport-accept atomicity regression passed");
    return 0;
}
