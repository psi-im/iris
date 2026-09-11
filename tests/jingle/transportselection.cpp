#include <QCoreApplication>
#include <iris/jingle-ice.h>
#include <iris/jingle-nstransportslist.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_caps.h>
#include <iris/xmpp_client.h>

using namespace XMPP;
using namespace XMPP::Jingle;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static void setPeerFeatures(Client &client, const Jid &peer, const QStringList &features)
{
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:jingle-transport-selection"));
    disco.setFeatures(Features(features));

    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

static QString selectTransport(Session &session)
{
    // NSTransportsList prefers the last namespace in the list.
    NSTransportsList selector(&session, { ICE::NS_ICE_UDP, ICE::NS });
    const auto       transport = selector.getNextTransport();
    return transport ? transport->pad()->ns() : QString();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);

    const Jid peer(QStringLiteral("peer@example.org/device"));
    Session   session(client.jingleManager(), peer);

    setPeerFeatures(client, peer, { ICE::NS, ICE::NS_ICE_UDP });
    check(selectTransport(session) == ICE::NS, "ice:0 was not preferred when both profiles were available");

    setPeerFeatures(client, peer, { ICE::NS_ICE_UDP });
    check(selectTransport(session) == ICE::NS_ICE_UDP, "ice-udp:1-only peer was rejected");

    setPeerFeatures(client, peer, { ICE::NS });
    check(selectTransport(session) == ICE::NS, "ice:0-only peer was rejected");

    setPeerFeatures(client, peer, {});
    check(selectTransport(session).isEmpty(), "transport selected without a matching peer capability");

    qInfo("Jingle transport selection regressions passed");
}
