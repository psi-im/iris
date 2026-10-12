// SPDX-License-Identifier: LGPL-2.1-or-later
// SXE payload on an ICE SCTP data channel between two clients, and the fallback to SXE messages
// when the channel breaks. Reuses the looped-back two-client fixture; ICE runs on localhost.
#define main iris_sxesession_main
#include "sxesession.cpp"
#undef main

#include <iris/jingle-ice.h>
#include <iris/tcpportreserver.h>
#include <iris/xmpp_caps.h>

#include <QElapsedTimer>
#include <QThread>

#include <functional>

static void setPeerFeatures(Client &client, const Jid &peer, QStringList features)
{
    features.removeDuplicates();
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:jingle-sxe-connection"));
    disco.setFeatures(Features(features));
    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

// Exchanges stanzas and lets the localhost ICE and SCTP traffic run until done() or the timeout.
static bool pumpUntil(Endpoint &a, Endpoint &b, const std::function<bool()> &done, int timeoutMs = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        pump(a, b);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    pump(a, b);
    return done();
}

static int sxeMessages()
{
    int count = 0;
    for (const auto &message : std::as_const(sentMessages))
        if (!message.firstChildElement(QStringLiteral("sxe")).isNull())
            ++count;
    return count;
}

static J::Connection::Ptr dataChannel(SXE::Application *application)
{
    if (!application->transport())
        return {};
    for (const auto &channel : application->transport()->channels())
        if (channel && channel->features() & J::TransportFeature::MessageOriented
            && channel->features() & J::TransportFeature::DataOriented)
            return channel;
    return {};
}

static bool synchronized(SXE::Application *a, SXE::Application *b)
{
    return a && b && a->syncState() == SXE::Application::SyncState::Synchronized
        && b->syncState() == SXE::Application::SyncState::Synchronized;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    TcpPortReserver  reserver;

    Endpoint alice(QStringLiteral("alice"));
    Endpoint bob(QStringLiteral("bob"));
    for (auto endpoint : { &alice, &bob }) {
        endpoint->client.setTcpPortReserver(&reserver);
        endpoint->client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    }
    const auto features = alice.client.jingleManager()->discoFeatures();
    if (!features.contains(SXE::NS_CONNECTIONS)) {
        // Without SCTP over ICE (or DTLS) there is no connection transport to test.
        qInfo("SXE connection tests skipped: no transport can carry SXE messages");
        return 0;
    }
    setPeerFeatures(alice.client, bob.jid, bob.client.jingleManager()->discoFeatures());
    setPeerFeatures(bob.client, alice.jid, features);

    QPointer<J::Session> bobSession;
    QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &app,
                     [&](J::Session *session) { bobSession = session; });

    auto session = alice.client.jingleManager()->newSession(bob.jid);
    auto content = qobject_cast<SXE::Application *>(session->newContent(SXE::NS_SVG_WHITEBOARD, J::Origin::Both));
    check(content && content->isHost(), "whiteboard content was not created");
    session->addContent(content);
    session->initiate();
    pump(alice, bob);
    check(bobSession, "incoming whiteboard session not offered");
    auto remote = qobject_cast<SXE::Application *>(
        bobSession->contentList().value(J::ContentKey { content->contentName(), J::Origin::Initiator }));
    check(remote, "incoming whiteboard content missing");
    remote->prepare();
    bobSession->accept();

    check(pumpUntil(alice, bob, [&] { return synchronized(content, remote); }),
          "whiteboard did not synchronize over the connection");
    check(content->transport()->pad()->ns() != SXE::NS_TRANSPORT
              && remote->transport()->pad()->ns() == content->transport()->pad()->ns(),
          "a connection transport was not selected");
    check(sxeMessages() == 0, "SXE payload went through messages although a connection was available");
    check(dump(remote->document()->document()) == dump(content->document()->document()),
          "synchronized documents differ");

    // Edits in both directions, including character data far beyond one message of the message transport.
    QDomDocument temp;
    content->document()->insertNodeAfter(
        parse(temp, QStringLiteral("<rect xmlns='http://www.w3.org/2000/svg' id='r' width='5' height='5'/>")),
        content->document()->document().documentElement());
    content->document()->flush();
    const auto big  = QString(300000, QLatin1Char('x'));
    auto       rect = content->document()->document().documentElement().lastChildElement();
    content->document()->setAttribute(rect, QStringLiteral("data-big"), big);
    content->document()->flush();
    remote->document()->insertNodeAfter(
        parse(temp, QStringLiteral("<circle xmlns='http://www.w3.org/2000/svg' id='c' r='3'/>")),
        remote->document()->document().documentElement());
    remote->document()->flush();
    check(pumpUntil(alice, bob,
                    [&] {
                        return dump(remote->document()->document()) == dump(content->document()->document())
                            && !remote->document()->document().documentElement().lastChildElement().isNull()
                            && content->document()->document().elementsByTagName(QStringLiteral("circle")).size() == 1;
                    }),
          "edits over the connection were not exchanged");
    check(sxeMessages() == 0, "edits went through messages");

    // The channel breaks: the content moves to the next transport, which may be another connection
    // transport, and finally to SXE messages. The joiner synchronizes again after every replacement.
    for (int attempt = 0; content->transport()->pad()->ns() != SXE::NS_TRANSPORT; ++attempt) {
        check(attempt < 4, "the whiteboard never fell back to SXE messages");
        const auto failedNs = content->transport()->pad()->ns();
        auto       channel  = dataChannel(content);
        check(bool(channel), "the data channel of the whiteboard was not found");
        channel->close();
        check(pumpUntil(alice, bob,
                        [&] {
                            return content->transport() && remote->transport()
                                && content->transport()->pad()->ns() != failedNs
                                && remote->transport()->pad()->ns() == content->transport()->pad()->ns()
                                && synchronized(content, remote)
                                && (content->transport()->pad()->ns() == SXE::NS_TRANSPORT || dataChannel(content));
                        }),
              "the whiteboard did not move to the next transport");
        check(dump(remote->document()->document()) == dump(content->document()->document()),
              "documents differ after a transport replacement");
    }
    check(sxeMessages() > 0, "no SXE messages after the fallback");

    content->document()->insertNodeAfter(
        parse(temp, QStringLiteral("<line xmlns='http://www.w3.org/2000/svg' id='l' x2='9' y2='9'/>")),
        content->document()->document().documentElement());
    content->document()->flush();
    check(pumpUntil(alice, bob,
                    [&] { return dump(remote->document()->document()) == dump(content->document()->document()); }),
          "edits after the fallback were not exchanged");
    check(content->state() == J::State::Active && remote->state() == J::State::Active,
          "the whiteboard content did not stay active");

    session->terminate(J::Reason::Success);
    pumpUntil(alice, bob, [] { return false; }, 200);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qInfo("SXE connection and fallback tests passed");
    return 0;
}
