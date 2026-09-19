// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/jingle-s5b.cpp"
#include <QCoreApplication>
#include <QtCrypto>

using namespace XMPP;
namespace J = XMPP::Jingle;
namespace S = XMPP::Jingle::S5B;

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

static QDomElement payload(const QString &children)
{
    QDomDocument doc;
    check(bool(doc.setContent(QStringLiteral("<transport xmlns='urn:xmpp:jingle:transports:s5b:1' sid='test'>")
                                  + children + QStringLiteral("</transport>"),
                              true)),
          "Invalid test XML");
    return doc.documentElement();
}

namespace XMPP { namespace Jingle { namespace S5B {
    struct TransportTestAccess {
        static Candidate nominate(Transport &t, bool local, Candidate::State state, bool proxy = true)
        {
            auto el = payload(QStringLiteral("<candidate cid='chosen' host='127.0.0.1' port='54321'"
                                             " priority='100' jid='proxy.example.test' type='%1'/>")
                                  .arg(proxy ? QStringLiteral("proxy") : QStringLiteral("direct")))
                          .firstChildElement();
            Candidate candidate(&t, el);
            candidate.setState(state);
            auto &map            = local ? t.d->localCandidates : t.d->remoteCandidates;
            map[candidate.cid()] = candidate;
            (local ? t.d->localUsedCandidate : t.d->remoteUsedCandidate) = candidate;
            t._state                                                     = Jingle::State::Connecting;
            return candidate;
        }
        static void outgoingProxyError(Transport &t)
        {
            t.d->offerSent      = true;
            t.d->pendingActions = Transport::Private::ProxyError;
        }
        static size_t remoteCount(const Transport &t) { return t.d->remoteCandidates.size(); }
    };
}}}

struct Fixture {
    J::Session                   session;
    S::Manager                   manager;
    J::TransportManagerPad::Ptr  pad;
    QSharedPointer<S::Transport> transport;

    Fixture(Client &client, J::Origin role = J::Origin::Responder) :
        session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), role)
    {
        manager.setJingleManager(client.jingleManager());
        pad       = J::TransportManagerPad::Ptr(manager.pad(&session));
        transport = manager.newTransport(pad, session.peerRole()).staticCast<S::Transport>();
    }
};

static void testPreparation(Client &client)
{
    Fixture    f(client);
    const auto candidate = QStringLiteral("<candidate cid='a' host='127.0.0.1' jid='peer.example.test'"
                                          " port='54321' priority='100' type='direct'/>");
    for (const auto &contents :
         { candidate + QStringLiteral("<candidate-used/>"), QStringLiteral("<candidate-error/>") + candidate,
           QStringLiteral("<candidate-error/><candidate-error/>"), QStringLiteral("<activated cid='a'/><proxy-error/>"),
           candidate + QStringLiteral("<candidate cid='broken'/>"),
           QStringLiteral("<proxy-error><candidate-error/></proxy-error>") }) {
        check(!f.transport->prepareUpdate(payload(contents)), "Malformed payload was staged");
        check(f.transport->sid().isEmpty() && S::TransportTestAccess::remoteCount(*f.transport) == 0
                  && f.transport->state() == J::State::Created,
              "Rejected preparation mutated live transport");
    }
    auto prepared = f.transport->prepareUpdate(payload(candidate));
    check(bool(prepared) && S::TransportTestAccess::remoteCount(*f.transport) == 0,
          "Valid preparation has side effects");
    check(f.transport->commitPreparedUpdate(std::move(prepared.update)), "Valid commit failed");
    check(f.transport->sid() == QLatin1String("test") && S::TransportTestAccess::remoteCount(*f.transport) == 1,
          "Valid commit did not install candidates");

    check(bool(f.transport->prepareUpdate(payload(QStringLiteral("<candidate-used xmlns='urn:unknown'/>")))),
          "Foreign extension was interpreted as an S5B command");
}

static void testProxyError(Client &client, J::Origin role, bool local, S::Candidate::State state)
{
    Fixture f(client, role);
    auto    candidate = S::TransportTestAccess::nominate(*f.transport, local, state);
    int     failures  = 0;
    QObject::connect(f.transport.data(), &J::Transport::failed, &f.session, [&] { ++failures; });
    const auto xml      = payload(QStringLiteral("<proxy-error/>"));
    auto       prepared = f.transport->prepareUpdate(xml);
    check(bool(prepared) && candidate.state() == state && !failures, "Proxy error preparation failed or mutated state");
    check(f.transport->commitPreparedUpdate(std::move(prepared.update)), "Proxy error commit failed");
    check(f.transport->state() == J::State::Finishing && candidate.state() == S::Candidate::Discarded,
          "Proxy negotiation was not retired");
    check(!f.transport->update(xml), "Duplicate proxy error was accepted");
    QCoreApplication::processEvents();
    check(failures == 1 && f.transport->state() == J::State::Finished, "Proxy error did not signal fallback once");
    QCoreApplication::processEvents();
    check(failures == 1, "Proxy error signaled duplicate failure");
}

static void testStaleAndUnexpected(Client &client)
{
    Fixture    f(client);
    const auto xml = payload(QStringLiteral("<proxy-error/>"));
    check(!f.transport->prepareUpdate(xml), "Proxy error without nomination accepted");
    S::TransportTestAccess::nominate(*f.transport, false, S::Candidate::Accepted, false);
    check(!f.transport->prepareUpdate(xml), "Proxy error terminated direct candidate");
    S::TransportTestAccess::nominate(*f.transport, false, S::Candidate::Accepted);
    auto prepared = f.transport->prepareUpdate(xml);
    check(bool(prepared), "Could not stage proxy error");
    auto newer = S::TransportTestAccess::nominate(*f.transport, false, S::Candidate::Accepted);
    check(!f.transport->commitPreparedUpdate(std::move(prepared.update)), "Stale same-cid snapshot was committed");
    check(newer.state() == S::Candidate::Accepted, "Stale proxy error mutated successor");
}

class Success final : public Task {
public:
    explicit Success(Task *parent) : Task(parent) { setSuccess(); }
};

static void testOutgoingError(Client &client, S::Candidate::State state, bool crossed)
{
    Fixture f(client);
    S::TransportTestAccess::nominate(*f.transport, true, state);
    S::TransportTestAccess::outgoingProxyError(*f.transport);
    int failures = 0;
    QObject::connect(f.transport.data(), &J::Transport::failed, &f.session, [&] { ++failures; });
    auto [xml, ack] = f.transport->takeOutgoingUpdate(false);
    check(!xml.firstChildElement(QStringLiteral("proxy-error")).isNull() && bool(ack), "Missing outgoing proxy error");
    if (crossed)
        check(f.transport->update(payload(QStringLiteral("<proxy-error/>"))), "Crossed proxy error rejected");
    Success success(client.rootTask());
    ack(&success);
    ack(&success);
    QCoreApplication::processEvents();
    check(failures == 1 && f.transport->state() == J::State::Finished,
          "Proxy-error ACK did not finish exact attempt once");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    TcpPortReserver  reserver;
    client.setTcpPortReserver(&reserver);
    testPreparation(client);
    for (auto role : { J::Origin::Initiator, J::Origin::Responder }) {
        testProxyError(client, role, false, S::Candidate::Accepted);
        testProxyError(client, role, true, S::Candidate::Accepted);
        testProxyError(client, role, true, S::Candidate::Activating);
    }
    testStaleAndUnexpected(client);
    testOutgoingError(client, S::Candidate::Activating, false);
    testOutgoingError(client, S::Candidate::Discarded, false);
    testOutgoingError(client, S::Candidate::Activating, true);
    qInfo("S5B staged payload and proxy failure regressions passed");
}
