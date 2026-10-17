// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0203: preserve the UTC instant and fractional seconds in MAM replay.
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>
#include <iris/xmpp_forwarding.h>
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
    OfflineConnector connector;
    RecordingStream stream(&connector);
    Client client;
    client.start(QStringLiteral("example.test"), QStringLiteral("alice"), QString(),
                 QStringLiteral("desktop"));
    client.connectToServer(&stream, Jid(QStringLiteral("alice@example.test/desktop")));

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

    QDomDocument legacyDocument;
    const auto legacyXml = parse(QStringLiteral(
        "<message xmlns='jabber:client' from='bob@example.test' type='chat'>"
        "<x xmlns='jabber:x:delay' stamp='20261016T12:00:00'/></message>"), legacyDocument);
    Message legacy;
    check(legacy.fromStanza(stream.createStanza(legacyXml)), "Cannot parse legacy delay");
    check(utc(legacy.timeStamp()) == QStringLiteral("2026-10-16T12:00:00.000Z"),
          "Legacy XEP-0091 UTC interpretation regressed");
    return 0;
}
