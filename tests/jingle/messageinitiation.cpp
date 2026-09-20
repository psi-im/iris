// SPDX-License-Identifier: LGPL-2.1-or-later
#include <iris/xmpp-im/jingle-message.h>
#include <iris/xmpp-im/jingle.h>
#include <iris/xmpp-im/xmpp_client.h>
#include <iris/xmpp-im/xmpp_jinglemessage.h>
#include <iris/xmpp-im/xmpp_message.h>

#include <QCoreApplication>
#include <QDomDocument>

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

static void churnDom()
{
    for (int i = 0; i < 128; ++i) {
        QDomDocument document;
        auto root = document.createElement(QStringLiteral("churn"));
        root.setAttribute(QStringLiteral("i"), i);
        document.appendChild(root);
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    {
        QDomDocument source;
        const auto root = parseRoot(
            QStringLiteral(
                "<propose xmlns='urn:xmpp:jingle-message:0' id='call-1'>"
                "<description xmlns='urn:xmpp:jingle:apps:rtp:1' media='audio'>"
                "<future xmlns='urn:iris:test' value='opaque'/>"
                "</description>"
                "<description xmlns='urn:xmpp:jingle:apps:rtp:1' media='video'/>"
                "<vendor xmlns='urn:iris:vendor' flag='1'/>"
                "</propose>"),
            &source);

        auto initiation = J::MessageInitiation::fromXml(root);
        check(initiation.isValid(), "valid JMI propose was rejected");
        check(initiation.action() == J::MessageInitiation::Action::Propose, "propose action was not parsed");
        check(initiation.id() == QStringLiteral("call-1"), "propose id was not parsed");
        check(initiation.descriptions().size() == 2, "propose descriptions were not preserved");
        check(initiation.descriptions().at(0).ns == QStringLiteral("urn:xmpp:jingle:apps:rtp:1"),
              "description namespace was not preserved");
        check(initiation.descriptions().at(0).media == QStringLiteral("audio"),
              "description media was not preserved");
        check(initiation.extensions().size() == 1, "unknown JMI extension was not preserved");

        source = QDomDocument();
        churnDom();

        QDomDocument target;
        const auto serialized = initiation.toXml(&target);
        check(!serialized.isNull(), "owned JMI propose could not be serialized after source destruction");
        target.appendChild(serialized);
        const auto audio = serialized.firstChildElement(QStringLiteral("description"));
        check(audio.namespaceURI() == QStringLiteral("urn:xmpp:jingle:apps:rtp:1"),
              "description namespace changed on round trip");
        check(audio.attribute(QStringLiteral("media")) == QStringLiteral("audio"),
              "description media changed on round trip");
        check(audio.firstChildElement(QStringLiteral("future")).namespaceURI() == QStringLiteral("urn:iris:test"),
              "opaque description XML was lost");
        check(serialized.lastChildElement(QStringLiteral("vendor")).namespaceURI() == QStringLiteral("urn:iris:vendor"),
              "opaque top-level JMI XML was lost");
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
        message.addJingleMessageInitiation(ringing);
        check(message.jingleMessageInitiations().size() == 1,
              "Message did not retain typed JMI payload");
        check(message.jingleMessageInitiations().first().action() == J::MessageInitiation::Action::Ringing,
              "Message changed typed JMI action");
    }

    {
        Client client;
        auto manager = client.jingleManager();
        auto jmi = manager->messageInitiationManager();
        check(jmi && !jmi->enabled(), "JMI manager must be opt-in");
        check(!manager->discoFeatures().contains(J::MessageInitiation::ns()),
              "disabled JMI was advertised");
        jmi->setEnabled(true);
        check(manager->discoFeatures().contains(J::MessageInitiation::ns()),
              "enabled JMI was not advertised");
        jmi->setEnabled(false);
        check(!manager->discoFeatures().contains(J::MessageInitiation::ns()),
              "disabled JMI remained advertised");
    }

    qInfo("XEP-0353 Jingle Message Initiation regressions passed");
    return 0;
}
