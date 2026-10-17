// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0203: preserve the UTC instant and fractional seconds in MAM replay.
#include "fixture.h"
#include <iris/xmpp_forwarding.h>
#include <iris/xmpp_message.h>
#include <iris/xmpp_status.h>
#include <iris/xmpp_tasks.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QtCrypto>

using namespace XMPP;
using namespace MessageTest;

static QDomElement parse(const QString &xml, QDomDocument &document)
{
    check(document.setContent(xml, true), "Invalid delay fixture");
    return document.documentElement();
}

static QString utc(const QDateTime &dateTime)
{
    return dateTime.toUTC().toString(Qt::ISODateWithMs);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Endpoint endpoint;
    auto &stream = endpoint.stream;
    auto &client = endpoint.client;

    QDomDocument source;
    const auto root = parse(QStringLiteral(
        "<message xmlns='jabber:client' from='bob@example.test' to='alice@example.test' "
        "type='chat' id='msg1'><body>Hi</body>"
        "<delay xmlns='urn:xmpp:delay' stamp='2026-10-16T12:00:00.250+02:00'/>"
        "</message>"), source);
    Message message;
    check(message.fromStanza(stream.createStanza(root)), "Cannot parse delayed message");
    check(utc(message.timeStamp()) == QStringLiteral("2026-10-16T10:00:00.250Z"),
          "XEP-0203 timestamp was interpreted as a local wall clock time");

    const auto outgoing = message.toStanza(&stream);
    check(!outgoing.isNull(), "Cannot serialize delayed message");
    const auto delay = outgoing.element().elementsByTagNameNS(QStringLiteral("urn:xmpp:delay"),
                                                              QStringLiteral("delay")).at(0).toElement();
    check(delay.attribute(QStringLiteral("stamp")) == QStringLiteral("2026-10-16T10:00:00.250Z"),
          "Serialized delay lost milliseconds or emitted an extra Z");

    QDomDocument forwardedDocument;
    const auto forwardedXml = parse(QStringLiteral(
        "<forwarded xmlns='urn:xmpp:forward:0'>"
        "<delay xmlns='urn:xmpp:delay' stamp='2026-10-16T12:00:00.250+02:00'/>"
        "<message xmlns='jabber:client' from='bob@example.test' type='chat'>"
        "<body>Archived</body></message></forwarded>"), forwardedDocument);
    Forwarding forwarded;
    check(forwarded.fromXml(forwardedXml, &client), "Cannot parse forwarded MAM message");
    check(utc(forwarded.timeStamp()) == QStringLiteral("2026-10-16T10:00:00.250Z"),
          "Forwarded timestamp lost the timezone offset");

    const auto forwardedOut = forwarded.toXml(&stream);
    check(!forwardedOut.isNull(), "Cannot serialize forwarded message");
    const auto forwardedDelay = forwardedOut.firstChildElement(QStringLiteral("delay"));
    check(forwardedDelay.attribute(QStringLiteral("stamp"))
              == QStringLiteral("2026-10-16T10:00:00.250Z"),
          "Forwarded delay serialization emitted an invalid UTC stamp");

    // Reusing the same decoder must not leak an earlier delay into a later
    // forwarded message that has no <delay/> element.
    QDomDocument withoutDelayDocument;
    const auto withoutDelay = parse(QStringLiteral(
        "<forwarded xmlns='urn:xmpp:forward:0'>"
        "<message xmlns='jabber:client' from='carol@example.test' type='chat'>"
        "<body>Fresh</body></message></forwarded>"), withoutDelayDocument);
    check(forwarded.fromXml(withoutDelay, &client), "Cannot parse a forwarded message without delay");
    check(!forwarded.timeStamp().isValid() && forwarded.message().body() == QStringLiteral("Fresh"),
          "Forwarding reused the timestamp or message from a previous envelope");

    // Expanded XML names are prefix-independent, including a prefixed
    // forwarding envelope around an ordinary client-namespace message.
    QDomDocument prefixedDocument;
    const auto prefixedForwarded = parse(QStringLiteral(
        "<f:forwarded xmlns:f='urn:xmpp:forward:0'>"
        "<message xmlns='jabber:client' from='carol@example.test' type='chat'>"
        "<body>Prefixed</body></message></f:forwarded>"), prefixedDocument);
    check(forwarded.fromXml(prefixedForwarded, &client)
              && forwarded.message().body() == QStringLiteral("Prefixed"),
          "Namespace-prefixed forwarded envelope was rejected");

    QDomDocument wrongNamespaceDocument;
    const auto wrongNamespace = parse(QStringLiteral(
        "<forwarded xmlns='urn:xmpp:forward:0'>"
        "<message xmlns='urn:other:0' from='carol@example.test' type='chat'>"
        "<body>Forged</body></message></forwarded>"), wrongNamespaceDocument);
    check(!forwarded.fromXml(wrongNamespace, &client),
          "Forwarded message in an unrelated XML namespace was accepted");

    QDomDocument legacyDocument;
    const auto legacyXml = parse(QStringLiteral(
        "<message xmlns='jabber:client' from='bob@example.test' type='chat'>"
        "<x xmlns='jabber:x:delay' stamp='20261016T12:00:00'/></message>"), legacyDocument);
    Message legacy;
    check(legacy.fromStanza(stream.createStanza(legacyXml)), "Cannot parse legacy delay");
    check(utc(legacy.timeStamp()) == QStringLiteral("2026-10-16T12:00:00.000Z"),
          "Legacy XEP-0091 UTC interpretation regressed");

    // XEP-0203 presence timestamps retain fractional seconds, offset,
    // and precedence over legacy XEP-0091 timestamps.
    JT_PushPresence presenceTask(client.rootTask());
    int presenceCount = 0;
    QDateTime presenceStamp;
    QObject::connect(&presenceTask, &JT_PushPresence::presence, &presenceTask,
                     [&](const Jid &, const Status &status) {
                         ++presenceCount;
                         presenceStamp = status.timeStamp();
                     });
    QDomDocument presenceDocument;
    const auto presenceXml = parse(QStringLiteral(
        "<presence from='room@conference.example.test/bob'>"
        "<x xmlns='jabber:x:delay' stamp='20261016T09:00:00'/>"
        "<delay xmlns='urn:xmpp:delay' stamp='2026-10-16T12:00:00.250+02:00'/>"
        "</presence>"), presenceDocument);
    check(presenceTask.take(presenceXml), "Cannot parse delayed presence");
    check(presenceCount == 1 && utc(presenceStamp) == QStringLiteral("2026-10-16T10:00:00.250Z"),
          "Presence lost XEP-0203 offset, fractions or modern-delay precedence");

    QDomDocument legacyPresenceDocument;
    const auto legacyPresence = parse(QStringLiteral(
        "<presence from='room@conference.example.test/bob'>"
        "<x xmlns='jabber:x:delay' stamp='20261016T12:00:00'/>"
        "</presence>"), legacyPresenceDocument);
    check(presenceTask.take(legacyPresence), "Cannot parse legacy delayed presence");
    check(presenceCount == 2 && utc(presenceStamp) == QStringLiteral("2026-10-16T12:00:00.000Z"),
          "Legacy presence delay was not interpreted as UTC");
    return 0;
}
