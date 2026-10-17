// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0359: preserve multiple assigning entities and avoid ambiguous IDs.
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>
#include <iris/xmpp_message.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QtCrypto>

using namespace XMPP;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class OfflineConnector final : public Connector {
public:
    void setOptHostPort(const QString &, quint16) override { }
    void connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void done() override { }
};

class RecordingStream final : public ClientStream {
public:
    explicit RecordingStream(Connector *connector) : ClientStream(connector) { }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    OfflineConnector connector;
    RecordingStream stream(&connector);
    Client client;
    client.start(QStringLiteral("example.test"), QStringLiteral("alice"), QString(),
                 QStringLiteral("desktop"));
    client.connectToServer(&stream, Jid(QStringLiteral("alice@example.test/desktop")));

    QDomDocument source;
    check(source.setContent(QStringLiteral(
              "<message xmlns='jabber:client' from='room@conference.example.test/alice' "
              "to='alice@example.test/desktop' type='groupchat' id='m1'>"
              "<body>Hello</body>"
              "<origin-id xmlns='urn:xmpp:sid:0' id='client-origin'/>"
              "<stanza-id xmlns='urn:xmpp:sid:0' id='room-id' by='room@conference.example.test'/>"
              "<stanza-id xmlns='urn:xmpp:sid:0' id='account-id' by='alice@example.test'/>"
              "<stanza-id xmlns='urn:xmpp:sid:0' id='forged-duplicate' by='room@conference.example.test'/>"
              "<stanza-id xmlns='urn:xmpp:sid:0' id='missing-by'/>"
              "<stanza-id xmlns='urn:xmpp:sid:0' by='server.example.test'/>"
              "</message>"), true), "Invalid stanza ID fixture");

    Message message;
    check(message.fromStanza(stream.createStanza(source.documentElement())), "Cannot parse message");
    check(message.stanzaIds().size() == 2, "Distinct stanza IDs were lost or invalid IDs accepted");
    check(message.stanzaId(Jid(QStringLiteral("room@conference.example.test"))).id
              == QStringLiteral("room-id"), "Room stanza ID was overwritten");
    check(message.stanzaId(Jid(QStringLiteral("alice@example.test"))).id
              == QStringLiteral("account-id"), "Account stanza ID was lost");
    check(message.stanzaId(Jid(QStringLiteral("untrusted.example.test"))).id.isEmpty(),
          "Unknown assigning entity returned an ID");
    check(message.stanzaId().id == QStringLiteral("account-id"),
          "Legacy stanzaId() no longer selects the last distinct ID");
    check(message.originId() == QStringLiteral("client-origin"), "Origin ID was lost");

    const auto outgoing = message.toStanza(&stream);
    check(!outgoing.isNull(), "Could not serialize stanza IDs");
    const auto serializedIds
        = outgoing.element().elementsByTagNameNS(QStringLiteral("urn:xmpp:sid:0"),
                                                 QStringLiteral("stanza-id"));
    check(serializedIds.size() == 2, "Serialization did not preserve both stanza IDs");

    Message roundTrip;
    check(roundTrip.fromStanza(outgoing), "Cannot parse serialized stanza");
    check(roundTrip.stanzaIds().size() == 2, "Roundtrip lost a stanza ID");
    check(roundTrip.stanzaId(Jid(QStringLiteral("room@conference.example.test"))).id
              == QStringLiteral("room-id"), "Roundtrip changed the room ID");

    roundTrip.addStanzaId({ Jid(QStringLiteral("alice@example.test")), QStringLiteral("duplicate") });
    check(roundTrip.stanzaIds().size() == 2, "Duplicate assigning entity was accepted");
    roundTrip.setStanzaId({ Jid(QStringLiteral("new.example.test")), QStringLiteral("replacement") });
    check(roundTrip.stanzaIds().size() == 1
              && roundTrip.stanzaId().id == QStringLiteral("replacement"),
          "Legacy setStanzaId() failed to replace the list");
    return 0;
}
