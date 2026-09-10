#include <QCoreApplication>
#include <QDebug>
#include "jingle-ice-udp.h"

using XMPP::Jingle::ICE::NS_ICE_UDP;
using XMPP::Jingle::ICE::UdpTransportCodec;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static QDomElement xml(const QString &body)
{
    QDomDocument doc;
    check(doc.setContent(body, true), "invalid test XML");
    return doc.documentElement();
}

static std::optional<XMPP::Jingle::ICE::UdpTransportDescription> parse(const QString &body)
{
    return UdpTransportCodec::fromXml(
        xml("<transport xmlns='urn:xmpp:jingle:transports:ice-udp:1' pwd='secret' ufrag='frag'>" + body
            + "</transport>"));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const auto description = parse(
        "<candidate component='1' foundation='1' generation='0' id='host1' ip='192.0.2.1' network='0' "
        "port='5000' priority='2130706431' protocol='udp' type='host'/>"
        "<candidate component='1' foundation='2' generation='0' id='relay1' ip='192.0.2.2' port='5001' "
        "priority='1677734911' protocol='udp' rel-addr='192.0.2.1' rel-port='5000' type='relay'/>"
        "<fingerprint xmlns='urn:xmpp:jingle:apps:dtls:0' hash='sha-256' setup='actpass'>AA:BB</fingerprint>");
    check(description && description->candidates.size() == 2, "valid ICE-UDP description rejected");
    check(description->candidates.first().network == 0 && description->candidates.last().network == -1,
          "optional network attribute changed");
    check(description->extensions.size() == 1
              && description->extensions.first().namespaceURI() == QStringLiteral("urn:xmpp:jingle:apps:dtls:0"),
          "foreign transport extension lost");

    QDomDocument doc;
    const auto   transport = UdpTransportCodec::toXml(doc, *description);
    check(!transport.isNull() && transport.namespaceURI() == NS_ICE_UDP, "ICE-UDP serialization failed");
    doc.appendChild(transport);
    const auto roundtrip = UdpTransportCodec::fromXml(xml(doc.toString()));
    check(roundtrip && roundtrip->candidates.size() == 2 && roundtrip->extensions.size() == 1,
          "ICE-UDP roundtrip failed");

    const auto selected = parse("<remote-candidate component='1' ip='192.0.2.10' port='6000'/>");
    check(selected && selected->remoteCandidate && selected->remoteCandidate->port == 6000,
          "valid remote-candidate rejected");

    check(parse("").has_value(), "empty ICE-UDP transport rejected");
    check(!UdpTransportCodec::fromXml(xml(
              "<transport xmlns='urn:xmpp:jingle:transports:ice-udp:1'><candidate component='1' foundation='1' "
              "generation='0' id='x' ip='192.0.2.1' port='5000' priority='1' protocol='udp' type='host'/></transport>")),
          "candidate without credentials accepted");
    check(!parse("<candidate component='0' foundation='1' generation='0' id='x' ip='192.0.2.1' port='5000' "
                 "priority='1' protocol='udp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='256' id='x' ip='192.0.2.1' port='5000' "
                        "priority='1' protocol='udp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='not-an-ip' port='5000' "
                        "priority='1' protocol='udp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' port='0' "
                        "priority='1' protocol='udp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' port='5000' "
                        "priority='0' protocol='udp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' port='5000' "
                        "priority='1' protocol='tcp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' port='5000' "
                        "priority='1' protocol='udp' type='bogus'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' network='256' "
                        "port='5000' priority='1' protocol='udp' type='host'/>") ,
          "invalid ICE-UDP candidate accepted");

    check(!parse("<candidate component='1' foundation='1' generation='0' id='same' ip='192.0.2.1' port='5000' "
                 "priority='2' protocol='udp' type='host'/>"
                 "<candidate component='1' foundation='2' generation='0' id='same' ip='192.0.2.2' port='5001' "
                 "priority='1' protocol='udp' type='host'/>") ,
          "duplicate candidate id accepted");
    check(!parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' port='5000' "
                 "priority='1' protocol='udp' type='host'/>"
                 "<remote-candidate component='1' ip='192.0.2.2' port='5001'/>") ,
          "candidate and remote-candidate mixture accepted");
    check(!parse("<gathering-complete/>"), "XEP-0371 gathering-complete leaked into XEP-0176");
    check(!UdpTransportCodec::fromXml(xml("<transport xmlns='urn:xmpp:jingle:transports:ice:0'/>")).has_value(),
          "wrong ICE namespace accepted");

    qInfo("ICE-UDP codec regressions passed");
}
