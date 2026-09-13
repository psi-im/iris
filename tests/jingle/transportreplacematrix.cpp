// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Reuse the legacy transport-replace fixture in a second executable so the
// matrix below exercises exactly the same fake transports/selectors as the
// historical-behavior suite without duplicating several hundred lines of test
// plumbing. Rename its standalone entry point while including it here.
#define main iris_transportreplace_legacy_main
#include "transportreplace.cpp"
#undef main

static QDomElement makeUnqualifiedTransportReplace(QDomDocument &doc, const QString &name, J::Origin creator)
{
    auto jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    doc.appendChild(jingle);
    auto content = doc.createElementNS(J::NS, QStringLiteral("content"));
    J::ContentBase::setCreatorAttr(content, creator);
    content.setAttribute(QStringLiteral("name"), name);
    content.appendChild(doc.createElement(QStringLiteral("transport")));
    jingle.appendChild(content);
    return jingle;
}

static void testUnqualifiedTransportIsMalformed(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("plain@example.test/device")), J::Origin::Initiator);
    auto old = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("old"));
    auto selector = std::make_unique<TestSelector>();
    TestSelector *selectorRaw = nullptr;
    auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, old, std::move(selector),
                              &selectorRaw);

    QDomDocument doc;
    const bool ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeUnqualifiedTransportReplace(doc, QStringLiteral("audio"), J::Origin::Initiator));

    check(!ok, "transport-replace with an unqualified transport element was accepted");
    check(app->transport().data() == old.data(), "malformed unqualified transport changed current transport");
    check(selectorRaw->canReplaceCalls == 0 && selectorRaw->replaceCalls == 0,
          "malformed unqualified transport reached the selector");
}

static void testSelectorReplaceFailureKeepsCurrentTransport(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("replace-false@example.test/device")),
                       J::Origin::Initiator);
    auto old = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("old"));
    auto selector = std::make_unique<TestSelector>();
    selector->allowReplace = false;
    TestSelector *selectorRaw = nullptr;
    auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, old, std::move(selector),
                              &selectorRaw);

    QDomDocument doc;
    const bool ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc, { { QStringLiteral("audio"), J::Origin::Initiator,
                             TestTransportManager::namespaceUri(), QStringLiteral("remote") } }));

    check(ok, "selector replace failure rejected the whole transport-replace action");
    check(app->transport().data() == old.data(), "selector replace failure changed current transport");
    check(app->replaceIdle(), "selector replace failure changed replacement transaction state");
    check(selectorRaw->canReplaceCalls == 1 && selectorRaw->replaceCalls == 1,
          "selector replace failure did not follow canReplace -> replace path");
}

static void testMixedBatchAcceptsSupportedAndRejectsUnsupported(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("mixed@example.test/device")), J::Origin::Initiator);

    auto audioOld = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("audio-old"));
    auto audioSelector = std::make_unique<TestSelector>();
    TestSelector *audioSelectorRaw = nullptr;
    auto audio = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audioOld,
                                std::move(audioSelector), &audioSelectorRaw);

    auto videoOld = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("video-old"));
    auto videoSelector = std::make_unique<TestSelector>();
    videoSelector->allowCanReplace = false;
    TestSelector *videoSelectorRaw = nullptr;
    auto video = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, videoOld,
                                std::move(videoSelector), &videoSelectorRaw);

    QDomDocument doc;
    const bool ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc,
                    { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                        QStringLiteral("audio-remote") },
                      { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                        QStringLiteral("video-remote") } }));

    check(ok && !isTieBreak(session), "mixed supported/unsupported transport-replace batch was rejected wholesale");
    check(audio->transport().data() != audioOld.data() && audio->replaceInProgress(),
          "supported member of mixed batch was not installed");
    check(static_cast<TestTransport *>(audio->transport().data())->id() == QLatin1String("audio-remote"),
          "supported member of mixed batch installed the wrong transport");
    check(video->transport().data() == videoOld.data() && video->replaceIdle(),
          "unsupported member of mixed batch changed transport state");
    check(audioSelectorRaw->canReplaceCalls == 1 && audioSelectorRaw->replaceCalls == 1,
          "supported member of mixed batch did not traverse selector");
    check(videoSelectorRaw->canReplaceCalls == 1 && videoSelectorRaw->replaceCalls == 0,
          "unsupported member of mixed batch reached selector replace");
}

static void testPlannedLocalReplaceDoesNotTieBreak(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("planned@example.test/device")), J::Origin::Initiator);
    auto local = makeTransport(session, J::Origin::Initiator, J::State::ApprovedToSend, QStringLiteral("local-planned"));
    auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, local,
                              std::make_unique<TestSelector>());
    app->markReplacePlanned();

    QDomDocument doc;
    const bool ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc, { { QStringLiteral("audio"), J::Origin::Initiator,
                             TestTransportManager::namespaceUri(), QStringLiteral("remote") } }));

    check(ok && !isTieBreak(session), "unsent Planned replacement incorrectly won a tie-break");
    check(app->transport().data() != local.data() && app->replaceInProgress(),
          "peer replacement did not supersede an unsent Planned local replacement");
}

static void testAcknowledgedLocalReplaceDoesNotTieBreak(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("in-progress@example.test/device")),
                       J::Origin::Initiator);
    auto local = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("local-acked"));
    auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, local,
                              std::make_unique<TestSelector>());
    app->markReplaceInProgress();

    QDomDocument doc;
    const bool ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc, { { QStringLiteral("audio"), J::Origin::Initiator,
                             TestTransportManager::namespaceUri(), QStringLiteral("remote") } }));

    check(ok && !isTieBreak(session), "already-acknowledged local replacement incorrectly caused tie-break");
    check(app->transport().data() != local.data() && app->replaceInProgress(),
          "peer replacement was not installed after local transport-replace ACK");
}

static void testResponderNeedAckStillYieldsToInitiator(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("responder-race@example.test/device")),
                       J::Origin::Responder);
    auto local = makeTransport(session, J::Origin::Responder, J::State::ApprovedToSend, QStringLiteral("responder-local"));
    auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Responder, local,
                              std::make_unique<TestSelector>());
    app->markReplaceAwaitingAck();

    QDomDocument doc;
    const bool ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc, { { QStringLiteral("audio"), J::Origin::Responder,
                             TestTransportManager::namespaceUri(), QStringLiteral("initiator-winner") } }));

    check(ok && !isTieBreak(session), "responder rejected initiator winner while its own replace IQ was in flight");
    check(app->transport().data() != local.data() && app->transport()->creator() == J::Origin::Initiator,
          "responder did not install initiator winner during crossed transport-replace");
    check(app->replaceInProgress(), "responder winner install did not enter InProgress");
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCA::Initializer qca;
    TestTransportManager transportManager;
    Client client;
    client.jingleManager()->registerTransport(&transportManager);

    testUnqualifiedTransportIsMalformed(client);
    testSelectorReplaceFailureKeepsCurrentTransport(client);
    testMixedBatchAcceptsSupportedAndRejectsUnsupported(client);
    testPlannedLocalReplaceDoesNotTieBreak(client);
    testAcknowledgedLocalReplaceDoesNotTieBreak(client);
    testResponderNeedAckStillYieldsToInitiator(client);

    qInfo("Transport-replace state matrix regressions passed");
}
