// SPDX-License-Identifier: LGPL-2.1-or-later
// MAM wire-level regression with the real Task dispatcher and a recording stream.
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>
#include <iris/xmpp_features.h>
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
    require(baseline.archive().isEmpty(), "Forged result was accepted");
    result(c, first, QStringLiteral("alice@example.test"), QStringLiteral("m2"));
    result(c, first, QStringLiteral("alice@example.test"), QStringLiteral("m3"));
    require(finished == 0 && baseline.archive().size() == 2, "Page completed before fin");
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

    MAMTask muc(c.rootTask());
    const Jid room(QStringLiteral("room@conference.example.test"));
    muc.setArchiveJid(room);
    muc.get(Jid(), QDateTime(), QDateTime(), true, 10, 0, true, false);
    muc.go();
    auto roomIq = stream.sent.last();
    require(roomIq.attribute(QStringLiteral("to")) == room.full(), "MUC query not addressed to room");
    require(!hasField(roomIq, QStringLiteral("with")), "MUC room used as with filter");
    result(c, roomIq, QStringLiteral("other@example.test"), QStringLiteral("fake"));
    require(muc.archive().isEmpty(), "Forged room result accepted");
    result(c, roomIq, room.full(), QStringLiteral("room1"));
    fin(c, roomIq, room.full(), QStringLiteral("room1"), QStringLiteral("room1"), true);
    require(muc.success() && muc.archive().size() == 1, "Room archive failed");

    MAMTask id(c.rootTask());
    id.get(Jid(), QStringLiteral("m1"), QString(), false, 10, 0, false, false);
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
    return 0;
}
