#include <QCoreApplication>
#include <QDebug>
#include <iris/jingle-rtp-description.h>
using XMPP::Jingle::RTP::Description;

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
static std::optional<Description> parse(const QString &body, bool advisory = false)
{
    return Description::fromXml(
        xml("<description xmlns='urn:xmpp:jingle:apps:rtp:1' media='audio'>" + body + "</description>"), advisory);
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto       description = parse(
        "<payload-type id='111' name='opus' clockrate='48000' channels='2'>"
              "<parameter name='minptime' value='10'/><parameter name='useinbandfec' value='1'/>"
              "<rtcp-fb xmlns='urn:xmpp:jingle:apps:rtp:rtcp-fb:0' type='transport-cc'/>"
              "</payload-type><payload-type id='0'/><rtcp-mux/>"
              "<rtp-hdrext xmlns='urn:xmpp:jingle:apps:rtp:rtp-hdrext:0' id='1' uri='urn:ietf:params:rtp-hdrext:sdes:mid'/>");
    check(description && description->payloads.size() == 2, "valid description rejected");
    check(description->payloads.first().id == 111 && description->payloads.last().id == 0, "codec preference lost");
    check(description->payloads.first().clockrate == 48000 && description->payloads.first().channels == 2,
          "Opus parameters lost");
    check(description->payloads.last().channels.value_or(1) == 1, "default channel count changed");
    QDomDocument doc;
    doc.appendChild(description->toXml(doc));
    const auto roundtrip = Description::fromXml(xml(doc.toString()));
    check(roundtrip && roundtrip->rtcpMux && roundtrip->extensions.size() == 1
              && roundtrip->payloads.first().extensions.size() == 1,
          "extensions lost on roundtrip");
    check(!parse("<payload-type id='128' name='opus'/>") && !parse("<payload-type id='-1'/>")
              && !parse("<payload-type id='0'/><payload-type id='0'/>") && !parse("<payload-type id='111'/>")
              && !parse("<payload-type id='0' clockrate='4294967296'/>")
              && !parse("<payload-type id='0' channels='0'/>") && !parse(""),
          "invalid payload accepted");
    check(!parse("<payload-type id='0'><parameter name='x' value='1'/><parameter name='x' value='2'/></payload-type>"),
          "ambiguous fmtp accepted");
    const auto hint = parse("<payload-type id='111'><parameter name='x' value='1'/></payload-type>", true);
    check(hint && !hint->payloads.first().channels && !hint->payloads.first().clockrate,
          "advisory update invented omitted parameters");
    qInfo("RTP description regressions passed");
}
