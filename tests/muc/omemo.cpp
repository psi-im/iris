// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0384 encrypted MUC identity/SCE and own-message echo regressions.
#include "../omemo/fixture.h"

namespace {
void exerciseMuc(Endpoint &alice, Endpoint &bob)
{
    const Jid room(QStringLiteral("garden@conference.example.test"));
    const Jid occupant(room.full() + QStringLiteral("/Alice"));

    const auto encryptMessage = [&]() {
        QDomDocument doc;
        auto stanza = doc.createElement(QStringLiteral("message"));
        stanza.setAttribute(QStringLiteral("type"), QStringLiteral("groupchat"));
        stanza.setAttribute(QStringLiteral("from"), alice.jid.full());
        stanza.setAttribute(QStringLiteral("to"), room.bare());
        doc.appendChild(stanza);
        auto body = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("body"));
        body.appendChild(doc.createTextNode(QStringLiteral("Private MUC message")));
        stanza.appendChild(body);

        EncryptionContext send;
        send.recipients = { bob.jid.bare() };
        send.options.insert(QStringLiteral("omemoProtocol"), QStringLiteral("omemo2"));
        auto job = wait(alice.client.encryptionManager()->encrypt(OmemoEncryption::methodId(), stanza, send),
                        alice, bob);
        check(job->success(), "MUC OMEMO 2 encryption failed");

        QDomDocument incomingDoc;
        auto incoming = incomingDoc.importNode(job->stanza(), true).toElement();
        incomingDoc.appendChild(incoming);
        incoming.setAttribute(QStringLiteral("from"), occupant.full());
        incoming.setAttribute(QStringLiteral("to"), bob.jid.full());
        return incomingDoc;
    };

    QDomDocument incomingDoc = encryptMessage();
    auto incoming = incomingDoc.documentElement();
    auto unknown = wait(bob.client.encryptionManager()->decrypt(incoming), alice, bob);
    check(!unknown->success() && unknown->error() == EncryptionJob::Error::AuthenticationFailed,
          "MUC ciphertext without a real sender mapping was accepted");

    EncryptionContext receive;
    receive.options.insert(QStringLiteral("mucRealSender"), alice.jid.bare());
    auto opened = wait(bob.client.encryptionManager()->decrypt(incoming, receive), alice, bob);
    check(opened->success(), "MUC message with authenticated real sender failed");
    check(opened->metadata().sender.bare() == alice.jid.bare()
              && opened->stanza().attribute(QStringLiteral("from")) == occupant.full()
              && opened->stanza().attribute(QStringLiteral("to")) == bob.jid.full()
              && opened->stanza().firstChildElement(QStringLiteral("body")).text()
                     == QStringLiteral("Private MUC message"),
          "MUC identity, original addresses or decrypted body were not preserved");

    auto wrongRoomDoc = encryptMessage();
    auto wrongRoom = wrongRoomDoc.documentElement();
    wrongRoom.setAttribute(QStringLiteral("from"), QStringLiteral("other@conference.example.test/Alice"));
    auto roomAttack = wait(bob.client.encryptionManager()->decrypt(wrongRoom, receive), alice, bob);
    check(!roomAttack->success() && roomAttack->error() == EncryptionJob::Error::AuthenticationFailed,
          "MUC SCE <to/> did not bind ciphertext to the original room");

    qInfo("OMEMO 2 authenticated MUC sender and room binding passed");
}

void exerciseMucSelfEcho(Endpoint &alice, Endpoint &bob)
{
    const Jid room(QStringLiteral("garden@conference.example.test"));
    const QString nick(QStringLiteral("Alice"));
    const QString selfOccupant = room.full() + QLatin1Char('/') + nick;

    check(alice.client.groupChatJoin(room.domain(), room.node(), nick), "Could not join test MUC");

    QDomDocument doc;
    auto presence = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("presence"));
    presence.setAttribute(QStringLiteral("from"), selfOccupant);
    doc.appendChild(presence);
    auto x = doc.createElementNS(QStringLiteral("http://jabber.org/protocol/muc#user"), QStringLiteral("x"));
    auto item = doc.createElementNS(QStringLiteral("http://jabber.org/protocol/muc#user"), QStringLiteral("item"));
    item.setAttribute(QStringLiteral("jid"), alice.jid.full());
    item.setAttribute(QStringLiteral("affiliation"), QStringLiteral("member"));
    item.setAttribute(QStringLiteral("role"), QStringLiteral("participant"));
    x.appendChild(item);
    presence.appendChild(x);
    alice.stream.inject(presence);

    int ownMessages = 0;
    int decryptionFailures = 0;
    QObject::connect(&alice.client, &Client::stanzaDecryptionFailed, &alice.client,
                     [&](const QString &, const Jid &, EncryptionJob::Error, const QString &,
                         const EncryptionMetadata &) { ++decryptionFailures; });
    QString receivedBody;
    bool encrypted = false;
    QObject::connect(&alice.client, &Client::messageReceived, &alice.client, [&](const Message &m) {
        if (m.type() == Message::Type::Groupchat) {
            ++ownMessages;
            receivedBody = m.body();
            encrypted = m.wasEncrypted();
        }
    });

    QDomDocument captured;
    onStanza = [&](const Endpoint &source, QDomElement &stanza) {
        if (source.jid == alice.jid && stanza.attribute(QStringLiteral("type")) == QLatin1String("groupchat")) {
            captured.appendChild(captured.importNode(stanza, true));
        }
    };

    Message outgoing(room);
    outgoing.setType(Message::Type::Groupchat);
    outgoing.setId(QStringLiteral("muc-echo-test-1"));
    outgoing.setBody(QStringLiteral("A protected groupchat message"));
    EncryptionContext context;
    context.recipients = { bob.jid.bare() };
    context.options.insert(QStringLiteral("omemoProtocol"), QStringLiteral("omemo2"));
    auto *job = wait(alice.client.sendMessageEncrypted(outgoing, OmemoEncryption::methodId(), context), alice, bob);
    check(job->success(), "Could not send a MUC encrypted stanza");
    check(!captured.documentElement().isNull(), "The encrypted groupchat stanza was not emitted");

    QDomDocument echoed;
    auto stanza = echoed.importNode(captured.documentElement(), true).toElement();
    echoed.appendChild(stanza);
    stanza.setAttribute(QStringLiteral("from"), selfOccupant);
    stanza.setAttribute(QStringLiteral("to"), alice.jid.full());

    // Matching room, nick and message id alone must not authenticate an
    // echo. The ciphertext and encrypted recipient keys must also match.
    QDomDocument forgedDoc;
    auto forged = forgedDoc.importNode(stanza, true).toElement();
    forgedDoc.appendChild(forged);
    auto encrypted = forged.firstChildElement(QStringLiteral("encrypted"));
    check(!encrypted.isNull(), "OMEMO encrypted element missing in echo test");
    encrypted.setAttribute(QStringLiteral("tampered"), QStringLiteral("yes"));
    alice.stream.inject(forged);
    check(ownMessages == 0 && decryptionFailures == 1,
          "MUC echoed a ciphertext not matching the locally sent message");

    alice.stream.inject(stanza);
    check(ownMessages == 1 && receivedBody == QStringLiteral("A protected groupchat message") && encrypted,
          "The outgoing OMEMO MUC echo was not delivered locally as an encrypted message");

    // A duplicate echo has no remaining local plaintext mapping and must not
    // be shown again; neither may a message with the wrong stanza ID.
    alice.stream.inject(stanza);
    check(ownMessages == 1 && decryptionFailures == 1,
          "A duplicate OMEMO MUC self echo was shown or reported as a decryption error");
    onStanza = {};
    qInfo("OMEMO 2 MUC self echo reconciliation passed");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    Endpoint alice(QStringLiteral("alice"));
    Endpoint bob(QStringLiteral("bob"));
    for (auto endpoint : { &alice, &bob }) {
        auto setUp = wait(endpoint->omemo->setUp(), alice, bob);
        check(setUp->success() && endpoint->omemo->isReady(), "OMEMO setup failed");
    }
    if (alice.omemo->supportedProtocols().testFlag(OmemoProtocol::Omemo2)) {
        exerciseMuc(alice, bob);
        exerciseMucSelfEcho(alice, bob);
    } else {
        qInfo("OMEMO MUC tests skipped: OMEMO 2 unavailable");
    }
    pep.clear();
    return 0;
}
