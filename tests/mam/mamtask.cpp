// SPDX-License-Identifier: LGPL-2.1-or-later
// MAM wire-level regression with the real Task dispatcher and a recording stream.
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>
#include <iris/xmpp_features.h>
#include <iris/xmpp_mammanager.h>
#include <iris/xmpp_mamtask.h>
#include <QCoreApplication>
#include <QDomDocument>
#include <QtCrypto>
using namespace XMPP;

static void require(bool ok, const char *message) { if (!ok) qFatal("%s", message); }

class OfflineConnector final : public Connector {
public:
    void setOptHostPort(const QString &, quint16) override {}
    void connectToServer(const QString &) override {}
    ByteStream *stream() const override { return nullptr; }
    void done() override {}
};
class Recorder final : public ClientStream {
public:
    explicit Recorder(Connector *c) : ClientStream(c) {}
    void write(const Stanza &s) override { sent.append(s.element().cloneNode(true).toElement()); }
    QList<QDomElement> sent;
};
static QDomElement xml(const QString &text)
{
    QDomDocument d;
    require(d.setContent(text, true), "Malformed XML fixture");
    return d.documentElement();
}
static QString qid(const QDomElement &iq)
{
    return iq.firstChildElement(QStringLiteral("query")).attribute(QStringLiteral("queryid"));
}
static QDomElement rsm(const QDomElement &iq)
{
    return iq.firstChildElement(QStringLiteral("query")).firstChildElement(QStringLiteral("set"));
}
static bool hasField(const QDomElement &iq, const QString &var)
{
    auto fields = iq.elementsByTagName(QStringLiteral("field"));
    for (int i = 0; i < fields.count(); ++i)
        if (fields.at(i).toElement().attribute(QStringLiteral("var")) == var) return true;
    return false;
}
static void result(Client &c, const QDomElement &iq, const QString &from, const QString &id)
{
    const QString stanza = QStringLiteral(
        "<message from='%1'><result xmlns='urn:xmpp:mam:2' queryid='%2' id='%3'>"
        "<forwarded xmlns='urn:xmpp:forward:0'><delay xmlns='urn:xmpp:delay' "
        "stamp='2026-10-16T12:00:00Z'/><message xmlns='jabber:client' "
        "from='bob@example.test'><body>Hi</body></message></forwarded></result></message>"
    ).arg(from, qid(iq), id);
    require(c.rootTask()->take(xml(stanza)), "MAM result not consumed");
}
static void fin(Client &c, const QDomElement &iq, const QString &from,
                const QString &first, const QString &last, bool complete, bool stable = true)
{
    const QString stanza = QStringLiteral(
        "<iq from='%1' type='result' id='%2'><fin xmlns='urn:xmpp:mam:2' "
        "complete='%3' stable='%4'><set xmlns='http://jabber.org/protocol/rsm'>"
        "<first>%5</first><last>%6</last></set></fin></iq>"
    ).arg(from, iq.attribute(QStringLiteral("id")),
          complete ? QStringLiteral("true") : QStringLiteral("false"),
          stable ? QStringLiteral("true") : QStringLiteral("false"), first, last);
    require(c.rootTask()->take(xml(stanza)), "MAM fin not consumed");
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    OfflineConnector connector;
    Recorder stream(&connector);
    Client c;
    c.start(QStringLiteral("example.test"), QStringLiteral("alice"), QString(), QStringLiteral("desktop"));
    c.connectToServer(&stream, Jid(QStringLiteral("alice@example.test/desktop")));

    MAMTask baseline(c.rootTask());
    int finished = 0;
    QObject::connect(&baseline, &Task::finished, &baseline, [&] { ++finished; });
    baseline.get(Jid(), QDateTime(), QDateTime(), false, 2, 0, true, true);
    baseline.go();
    require(!stream.sent.isEmpty(), "No initial MAM query");
    auto first = stream.sent.last();
    require(first.attribute(QStringLiteral("to")).isEmpty(), "Own archive IQ has a to");
    require(!hasField(first, QStringLiteral("with")), "Unfiltered query has a with");
    require(!hasField(first, QStringLiteral("include-groupchat")), "Unadvertised field sent");
    require(first.firstChildElement(QStringLiteral("query"))
            .firstChildElement(QStringLiteral("flip-page")).isNull(), "Unadvertised flip-page sent");
    require(rsm(first).firstChildElement(QStringLiteral("max")).text() == QStringLiteral("2"),
            "Wrong page size");
    require(!rsm(first).firstChildElement(QStringLiteral("before")).isNull(), "Missing last-page cursor");
    result(c, first, QStringLiteral("other@example.test"), QStringLiteral("fake"));
    result(c, first, QStringLiteral("alice@example.test/other-device"), QStringLiteral("fake2"));
    require(baseline.archive().isEmpty(), "Forged result was accepted");
    const auto invalidDelay = QStringLiteral(
        "<message from='alice@example.test'><result xmlns='urn:xmpp:mam:2' queryid='%1' id='bad1'>"
        "<forwarded xmlns='urn:xmpp:forward:0'><delay xmlns='urn:xmpp:delay' "
        "stamp='not-a-date'/></forwarded></result></message>").arg(qid(first));
    require(c.rootTask()->take(xml(invalidDelay)), "Invalid delay result not consumed");
    const auto invalidMessage = QStringLiteral(
        "<message from='alice@example.test'><result xmlns='urn:xmpp:mam:2' queryid='%1' id='bad2'>"
        "<forwarded xmlns='urn:xmpp:forward:0'><message xmlns='urn:invalid:message'/>"
        "</forwarded></result></message>").arg(qid(first));
    require(c.rootTask()->take(xml(invalidMessage)), "Invalid message result not consumed");
    require(baseline.archive().isEmpty(), "Malformed forwarded result was accepted");
    result(c, first, QStringLiteral("alice@example.test"), QStringLiteral("m2"));
    result(c, first, QStringLiteral("alice@example.test"), QStringLiteral("m3"));
    require(finished == 0 && baseline.archive().size() == 2, "Page completed before fin");
    const int sentBeforeSpoofedFin = stream.sent.size();
    fin(c, first, QStringLiteral("alice@example.test/other-device"), QStringLiteral("m2"),
        QStringLiteral("m3"), true);
    require(finished == 0 && stream.sent.size() == sentBeforeSpoofedFin,
            "Forged IQ from another own resource completed the archive query");
    fin(c, first, QStringLiteral("alice@example.test"), QStringLiteral("m2"),
        QStringLiteral("m3"), false, false);
    auto second = stream.sent.last();
    require(qid(first) != qid(second), "Query ID was reused");
    require(rsm(second).firstChildElement(QStringLiteral("before")).text() == QStringLiteral("m2"),
            "Wrong backward cursor");
    result(c, second, QStringLiteral("alice@example.test"), QStringLiteral("m1"));
    fin(c, second, QStringLiteral("alice@example.test"), QStringLiteral("m1"),
        QStringLiteral("m1"), true);
    require(finished == 1 && baseline.success() && baseline.archive().size() == 3, "Pagination failed");
    require(baseline.isComplete() && !baseline.isStable(), "Fin flags not retained");

    // XEP-0313 tombstones retain their archive ID even without a message.
    // Missing delay and message are valid; malformed present elements are not.
    MAMTask optionalForwarding(c.rootTask());
    optionalForwarding.get(Jid(), QDateTime(), QDateTime(), false, 10, 0, false, false);
    optionalForwarding.go();
    const auto optionalIq = stream.sent.last();
    const auto sendOptional = [&](const QString &id, const QString &payload) {
        const auto stanza = QStringLiteral(
            "<message from='alice@example.test'><result xmlns='urn:xmpp:mam:2' queryid='%1' id='%2'>"
            "<forwarded xmlns='urn:xmpp:forward:0'>%3</forwarded></result></message>")
            .arg(qid(optionalIq)).arg(id).arg(payload);
        require(c.rootTask()->take(xml(stanza)), "Optional forwarded result not consumed");
    };
    sendOptional(QStringLiteral("deleted"), QStringLiteral(
        "<delay xmlns='urn:xmpp:delay' stamp='2026-10-16T12:00:00Z'/>"));
    sendOptional(QStringLiteral("undelayed"), QStringLiteral("<message xmlns='jabber:client'/>"));
    sendOptional(QStringLiteral("empty"), QString());
    require(optionalForwarding.archive().size() == 3, "Valid optional forwarding was rejected");
    require(optionalForwarding.archive().at(0).attribute(QStringLiteral("id")) == QStringLiteral("deleted"),
            "Tombstone archive UID was lost");
    require(optionalForwarding.archive().at(0).firstChildElement(QStringLiteral("forwarded"))
            .firstChildElement(QStringLiteral("message")).isNull(),
            "Tombstone acquired a fabricated message");
    fin(c, optionalIq, QStringLiteral("alice@example.test"), QStringLiteral("deleted"),
        QStringLiteral("empty"), true);
    require(optionalForwarding.success(), "Optional forwarding query failed");


    MAMTask muc(c.rootTask());
    const Jid room(QStringLiteral("room@conference.example.test"));
    muc.setArchiveJid(room);
    muc.get(Jid(), QDateTime(), QDateTime(), true, 10, 0, true, false);
    muc.go();
    auto roomIq = stream.sent.last();
    require(roomIq.attribute(QStringLiteral("to")) == room.full(), "MUC query not addressed to room");
    require(!hasField(roomIq, QStringLiteral("with")), "MUC room used as with filter");
    result(c, roomIq, QStringLiteral("other@example.test"), QStringLiteral("fake"));
    result(c, roomIq, QStringLiteral("room@conference.example.test/occupant"), QStringLiteral("fake2"));
    require(muc.archive().isEmpty(), "Forged room result accepted");
    result(c, roomIq, room.full(), QStringLiteral("room1"));
    fin(c, roomIq, room.full(), QStringLiteral("room1"), QStringLiteral("room1"), true);
    require(muc.success() && muc.archive().size() == 1, "Room archive failed");

    MAMTask id(c.rootTask());
    id.get(Jid(), QStringLiteral("m1"), QStringLiteral("m5"), false, 10, 0, false, false);
    auto count = stream.sent.size();
    id.go();
    require(!id.success() && stream.sent.size() == count, "Unadvertised extended filter sent");
    MAMTask extended(c.rootTask());
    Features features;
    features.addFeature(QStringLiteral("urn:xmpp:mam:2#extended"));
    extended.setArchiveFeatures(features);
    extended.get(Jid(), QStringLiteral("m1"), QString(), false, 10, 0, false, false);
    extended.go();
    require(hasField(stream.sent.last(), QStringLiteral("after-id")), "Extended ID filter missing");
    fin(c, stream.sent.last(), QStringLiteral("alice@example.test"), QString(), QString(), true);
    require(extended.success(), "Empty extended query did not finish");

    // Baseline RSM supports one-sided ID cursors without #extended.
    MAMTask fallback(c.rootTask());
    fallback.get(Jid(), QStringLiteral("m1"), QString(), false, 10, 0, false, false);
    fallback.go();
    auto after = stream.sent.last();
    require(!hasField(after, QStringLiteral("after-id")), "Fallback sent an extended field");
    require(rsm(after).firstChildElement(QStringLiteral("after")).text() == QStringLiteral("m1"),
            "Baseline forward ID cursor missing");
    fin(c, after, QStringLiteral("alice@example.test"), QString(), QString(), true);
    require(fallback.success(), "Baseline forward ID query failed");

    MAMTask before(c.rootTask());
    before.get(Jid(), QString(), QStringLiteral("m5"), false, 10, 0, false, true);
    before.go();
    auto prior = stream.sent.last();
    require(!hasField(prior, QStringLiteral("before-id")), "Fallback sent an extended field");
    require(rsm(prior).firstChildElement(QStringLiteral("before")).text() == QStringLiteral("m5"),
            "Baseline backward ID cursor missing");
    fin(c, prior, QStringLiteral("alice@example.test"), QString(), QString(), true);
    require(before.success(), "Baseline backward ID query failed");

    // The returned task is configurable before go(): reverse after-id
    // requires the archive's disco-advertised MAM #extended feature.
    MAMManager manager(&c);
    auto *unsupportedLatest = manager.getLatestMessagesFromArchive(
        Jid(), QStringLiteral("m1"), false, 10);
    const auto countBeforeUnsupported = stream.sent.size();
    unsupportedLatest->go();
    require(!unsupportedLatest->success() && stream.sent.size() == countBeforeUnsupported,
            "Reverse after-id silently used unsupported baseline RSM");

    auto *latest = manager.getLatestMessagesFromArchive(Jid(), QStringLiteral("m1"), false, 10);
    latest->setArchiveFeatures(features);
    latest->go();
    const auto latestIq = stream.sent.last();
    require(hasField(latestIq, QStringLiteral("after-id")), "Manager dropped extended after-id");
    require(!rsm(latestIq).firstChildElement(QStringLiteral("before")).isNull(),
            "Manager lost reverse pagination");
    fin(c, latestIq, QStringLiteral("alice@example.test"), QString(), QString(), true);
    require(latest->success(), "Manager extended reverse query did not finish");

    // Bounded pagination must cap the final RSM page without claiming the
    // archive is exhausted. A streaming consumer must not retain results.
    MAMTask bounded(c.rootTask());
    Features all;
    all.addFeature(QStringLiteral("urn:xmpp:mam:2#extended"));
    all.addFeature(QStringLiteral("urn:xmpp:mam:2#groupchat-field"));
    bounded.setArchiveFeatures(all);
    bounded.setCollectResults(false);
    int delivered = 0;
    QObject::connect(&bounded, &MAMTask::resultReceived, &bounded,
                     [&](const QDomElement &entry) {
                         require(!entry.attribute(QStringLiteral("id")).isEmpty(),
                                 "Streamed result has no archive id");
                         ++delivered;
                     });
    bounded.get(Jid(), QDateTime(), QDateTime(), false, 2, 3, true, false);
    bounded.go();
    auto page1 = stream.sent.last();
    require(hasField(page1, QStringLiteral("include-groupchat")), "Advertised groupchat field missing");
    require(!page1.firstChildElement(QStringLiteral("query"))
             .firstChildElement(QStringLiteral("flip-page")).isNull(), "Advertised flip-page missing");
    result(c, page1, QStringLiteral("alice@example.test"), QStringLiteral("b1"));
    result(c, page1, QStringLiteral("alice@example.test"), QStringLiteral("b2"));
    fin(c, page1, QStringLiteral("alice@example.test"), QStringLiteral("b1"),
        QStringLiteral("b2"), false);
    auto page2 = stream.sent.last();
    require(rsm(page2).firstChildElement(QStringLiteral("max")).text() == QStringLiteral("1"),
            "Final RSM page exceeds remaining cap");
    require(rsm(page2).firstChildElement(QStringLiteral("after")).text() == QStringLiteral("b2"),
            "Wrong forward cursor");
    result(c, page2, QStringLiteral("alice@example.test"), QStringLiteral("b3"));
    fin(c, page2, QStringLiteral("alice@example.test"), QStringLiteral("b3"),
        QStringLiteral("b3"), false);
    require(bounded.success() && !bounded.isComplete() && delivered == 3,
            "Bounded streaming query did not terminate correctly");
    require(bounded.archive().isEmpty(), "Streaming query retained buffered results");
    require(!c.rootTask()->take(xml(QStringLiteral(
                "<message from='alice@example.test'><result xmlns='urn:xmpp:mam:2' "
                "queryid='no-open-query' id='late'/></message>"))),
            "Unsolicited MAM result was handled as an ordinary message");

    MAMTask failed(c.rootTask());
    failed.get(Jid(), QDateTime(), QDateTime(), false);
    failed.go();
    const auto errorQuery = stream.sent.last();
    const auto errorIq = QStringLiteral(
        "<iq from='alice@example.test' type='error' id='%1'><error type='cancel'>"
        "<item-not-found xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
        "</error></iq>").arg(errorQuery.attribute(QStringLiteral("id")));
    require(c.rootTask()->take(xml(errorIq)), "MAM IQ error was not consumed");
    require(!failed.success() && !failed.statusString().isEmpty(), "MAM IQ error was not propagated");

    MAMTask loop(c.rootTask());
    loop.get(Jid(), QDateTime(), QDateTime(), false, 2, 0, false, false);
    loop.go();
    const auto loopPage1 = stream.sent.last();
    fin(c, loopPage1, QStringLiteral("alice@example.test"), QStringLiteral("same"),
        QStringLiteral("same"), false);
    const auto loopPage2 = stream.sent.last();
    fin(c, loopPage2, QStringLiteral("alice@example.test"), QStringLiteral("same"),
        QStringLiteral("same"), false);
    require(!loop.success() && !loop.statusString().isEmpty(), "Repeated cursor caused an infinite loop");
    return 0;
}
