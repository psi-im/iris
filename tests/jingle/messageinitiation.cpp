// SPDX-License-Identifier: LGPL-2.1-or-later
#include <iris/xmpp-im/jingle-rtp.h>
#include <iris/xmpp-im/jingle.h>
#include <iris/xmpp-im/xmpp_client.h>
#include <iris/xmpp-im/xmpp_jinglemessage.h>
#include <iris/xmpp-im/xmpp_message.h>

#include <QCoreApplication>
#include <QDomDocument>

#include <any>
#include <optional>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

static QDomElement parseRoot(const QString &xml, QDomDocument *document)
{
    check(document->setContent(xml, true), "could not parse JMI fixture");
    return document->documentElement();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    J::RTP::Manager rtp;

    {
        QDomDocument source;
        const auto root = parseRoot(
            QStringLiteral(
                "<propose xmlns='urn:xmpp:jingle-message:0' id='call-1'>"
                "<description xmlns='urn:xmpp:jingle:apps:rtp:1' media='audio'>"
                "<future xmlns='urn:iris:test' value='application-owned'/>"
                "</description>"
                "<proposal xmlns='urn:example:jingle:application'>"
                "<nested value='opaque-to-jmi'/>"
                "</proposal>"
                "</propose>"),
            &source);

        auto initiation = J::MessageInitiation::fromXml(
            root, [&rtp](const QDomElement &description) -> std::optional<std::any> {
                if (description.namespaceURI() == J::RTP::Description::ns())
                    return rtp.parseProposal(description);
                return std::nullopt;
            });

        check(initiation.isValid(), "valid JMI propose was rejected");
        check(initiation.action() == J::MessageInitiation::Action::Propose, "propose action was not parsed");
        check(initiation.id() == QStringLiteral("call-1"), "propose id was not parsed");
        check(initiation.descriptions().size() == 2, "proposal descriptions were not retained");

        const auto rtpDescription = initiation.descriptions().at(0);
        check(rtpDescription.applicationNamespace == J::RTP::Description::ns(),
              "RTP proposal namespace was not retained");
        check(rtpDescription.isSupported(), "registered RTP proposal was not parsed");
        const auto rtpProposal = std::any_cast<J::RTP::Proposal>(rtpDescription.data);
        check(rtpProposal.media == QStringLiteral("audio"), "RTP proposal media was not parsed by RTP manager");

        const auto unknown = initiation.descriptions().at(1);
        check(unknown.applicationNamespace == QStringLiteral("urn:example:jingle:application"),
              "unknown proposal namespace was not retained");
        check(!unknown.isSupported(), "unknown proposal unexpectedly acquired a typed payload");

        source = QDomDocument();
        check(std::any_cast<J::RTP::Proposal>(initiation.descriptions().at(0).data).media
                  == QStringLiteral("audio"),
              "typed RTP proposal depended on source DOM lifetime");
    }

    {
        J::MessageInitiation initiation(J::MessageInitiation::Action::Propose, QStringLiteral("call-out"));
        initiation.addDescription(J::RTP::Description::ns(), J::RTP::Proposal { QStringLiteral("video") });

        QDomDocument target;
        const auto serialized = initiation.toXml(
            &target, [&rtp](const QString &ns, const std::any &data, QDomDocument *document) {
                return ns == J::RTP::Description::ns() ? rtp.serializeProposal(data, document) : QDomElement();
            });
        check(!serialized.isNull(), "typed RTP proposal could not be serialized");
        const auto description = serialized.firstChildElement();
        check(description.namespaceURI() == J::RTP::Description::ns(),
              "serialized RTP proposal namespace changed");
        check(description.attribute(QStringLiteral("media")) == QStringLiteral("video"),
              "serialized RTP proposal media changed");
    }

    {
        QDomDocument document;
        auto unsupported = J::MessageInitiation::fromXml(
            parseRoot(
                QStringLiteral(
                    "<propose xmlns='urn:xmpp:jingle-message:0' id='unknown'>"
                    "<payload xmlns='urn:example:unknown'><nested/></payload>"
                    "</propose>"),
                &document));
        check(unsupported.isValid(), "unknown application proposal should remain structurally valid");
        check(unsupported.descriptions().size() == 1 && !unsupported.descriptions().at(0).isSupported(),
              "unknown application proposal was not marked unsupported");

        QDomDocument output;
        check(unsupported.toXml(&output).isNull(),
              "unsupported proposal must not be re-emitted by inventing application XML");
    }

    {
        QDomDocument document;
        auto reject = J::MessageInitiation::fromXml(parseRoot(
            QStringLiteral(
                "<reject xmlns='urn:xmpp:jingle-message:0' id='call-2'>"
                "<reason xmlns='urn:xmpp:jingle:1'><busy/><text>Already in a call</text></reason>"
                "<tie-break xmlns='urn:xmpp:jingle-message:0'/>"
                "<migrated xmlns='urn:xmpp:jingle-message:0' to='call-3'/>"
                "</reject>"),
            &document));
        check(reject.isValid(), "valid JMI reject was rejected");
        check(reject.action() == J::MessageInitiation::Action::Reject, "reject action was not parsed");
        check(reject.reasonCondition() == QStringLiteral("busy"), "reject reason was not parsed");
        check(reject.reasonText() == QStringLiteral("Already in a call"), "reject reason text was not parsed");
        check(reject.tieBreak(), "tie-break marker was not parsed");
        check(reject.migratedTo() == QStringLiteral("call-3"), "migrated target was not parsed");

        QDomDocument output;
        check(!reject.toXml(&output).isNull(), "non-proposal JMI unexpectedly required an application serializer");
    }

    {
        QDomDocument document;
        auto invalid = J::MessageInitiation::fromXml(
            parseRoot(QStringLiteral("<propose xmlns='urn:xmpp:jingle-message:0' id='empty'/>"), &document));
        check(!invalid.isValid(), "description-less propose was accepted");
    }

    {
        Message message;
        J::MessageInitiation ringing(J::MessageInitiation::Action::Ringing, QStringLiteral("call-4"));
        message.setJingleMessageInitiation(ringing);
        check(message.jingleMessageInitiation().isValid(), "Message did not retain typed JMI payload");
        check(message.jingleMessageInitiation().action() == J::MessageInitiation::Action::Ringing,
              "Message changed typed JMI action");
    }

    {
        Client client;
        auto manager = client.jingleManager();
        check(manager && !manager->messageInitiationEnabled(), "JMI must be opt-in");
        check(!manager->discoFeatures().contains(J::MessageInitiation::ns()),
              "disabled JMI was advertised");
        manager->setMessageInitiationEnabled(true);
        check(manager->discoFeatures().contains(J::MessageInitiation::ns()),
              "enabled JMI was not advertised");
        manager->setMessageInitiationEnabled(false);
        check(!manager->discoFeatures().contains(J::MessageInitiation::ns()),
              "disabled JMI remained advertised");

        J::MessageInitiation unsupported(J::MessageInitiation::Action::Propose, QStringLiteral("call-5"));
        unsupported.addDescription(QStringLiteral("urn:example:unknown"));
        check(!manager->sendMessageInitiation(Jid(QStringLiteral("peer@example.test")), unsupported),
              "manager sent a proposal that no application can serialize");
    }

    qInfo("XEP-0353 Jingle Message Initiation regressions passed");
    return 0;
}
