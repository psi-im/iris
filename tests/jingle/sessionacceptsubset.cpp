// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QEventLoop>
#include <QPointer>
#include <QSharedPointer>

#include <iris/jingle-application.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>
#include <qca.h>

#include <utility>

using namespace XMPP;
using namespace XMPP::Jingle;

static const QString applicationNs = QStringLiteral("urn:iris:test:subset-application");
static const QString transportNs   = QStringLiteral("urn:iris:test:subset-transport");

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

static void pump()
{
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
}

struct Stats {
    int starts  = 0;
    int removes = 0;
    int stops   = 0;
};

class TestApplicationPad final : public ApplicationManagerPad {
public:
    explicit TestApplicationPad(Session *session) : session_(session) { }

    Session            *session() const override { return session_; }
    QString             ns() const override { return applicationNs; }
    ApplicationManager *manager() const override { return nullptr; }
    QString             generateContentName(Origin) override { return {}; }

private:
    Session *session_ = nullptr;
};

class TestTransportPad final : public TransportManagerPad {
public:
    explicit TestTransportPad(Session *session) : session_(session) { }

    Session          *session() const override { return session_; }
    QString           ns() const override { return transportNs; }
    TransportManager *manager() const override { return nullptr; }

private:
    Session *session_ = nullptr;
};

class TestTransport final : public Transport {
public:
    TestTransport(Session *session, Origin creator, const QSharedPointer<Stats> &stats) :
        Transport(TransportManagerPad::Ptr(new TestTransportPad(session)), creator), stats_(stats)
    {
    }

    void prepare() override { setState(State::ApprovedToSend); }
    void start() override { setState(State::Active); }
    void stop() override
    {
        ++stats_->stops;
        Transport::stop();
    }
    bool update(const QDomElement &) override
    {
        setState(State::Accepted);
        return true;
    }
    bool                        hasUpdates() const override { return false; }
    OutgoingTransportInfoUpdate takeOutgoingUpdate(bool = false) override { return {}; }
    bool                        isValid() const override { return true; }
    TransportFeatures           features() const override { return TransportFeature::Reliable; }
    Connection::Ptr             addChannel(TransportFeatures, const QString &, int = -1) override { return {}; }
    QList<Connection::Ptr>      channels() const override { return {}; }

private:
    QSharedPointer<Stats> stats_;
};

class TestApplication final : public Application {
public:
    TestApplication(Session *session, QString name, const QSharedPointer<Stats> &stats) : stats_(stats)
    {
        _pad         = ApplicationManagerPad::Ptr(new TestApplicationPad(session));
        _contentName = std::move(name);
        _creator     = Origin::Initiator;
        _senders     = Origin::Both;
        _state       = State::Pending;
        _flags |= InitialApplication;
        _transport = QSharedPointer<TestTransport>::create(session, Origin::Initiator, stats_);
    }

    void setState(State state) override { _state = state; }
    const std::optional<Stanza::Error> &lastError() const override { return error_; }
    Reason                              lastReason() const override { return reason_; }

    SetDescError setRemoteOffer(const QDomElement &) override { return Unparsed; }
    SetDescError setRemoteAnswer(const QDomElement &) override
    {
        setState(State::Accepted);
        return Ok;
    }
    QDomElement makeLocalOffer() override { return {}; }
    QDomElement makeLocalAnswer() override { return {}; }
    Update      evaluateOutgoingUpdate() override { return { Action::NoAction, {} }; }
    void        prepare() override { }
    void start() override
    {
        ++stats_->starts;
        setState(State::Active);
    }
    void remove(Reason::Condition condition = Reason::Success, const QString &text = {}) override
    {
        reason_ = Reason(condition, text);
        setState(State::Finished);
    }
    void incomingRemove(const Reason &reason) override
    {
        reason_ = reason;
        ++stats_->removes;
        setState(State::Finished);
    }

protected:
    void prepareTransport() override { }

private:
    QSharedPointer<Stats>         stats_;
    std::optional<Stanza::Error> error_;
    Reason                        reason_;
};

static QDomElement answer(Application *accepted, bool malformed = false)
{
    QDomDocument doc;
    auto         jingle = doc.createElementNS(NS, QStringLiteral("jingle"));
    auto         content = doc.createElementNS(NS, QStringLiteral("content"));
    content.setAttribute(QStringLiteral("creator"), QStringLiteral("initiator"));
    content.setAttribute(QStringLiteral("name"), accepted->contentName());
    content.appendChild(doc.createElementNS(applicationNs, QStringLiteral("description")));
    content.appendChild(doc.createElementNS(transportNs, QStringLiteral("transport")));
    jingle.appendChild(content);

    if (malformed) {
        auto missing = doc.createElementNS(NS, QStringLiteral("content"));
        missing.setAttribute(QStringLiteral("creator"), QStringLiteral("initiator"));
        missing.setAttribute(QStringLiteral("name"), QStringLiteral("missing"));
        jingle.appendChild(missing);
    }
    return jingle;
}

static void addInitialPair(Session &session, const QSharedPointer<Stats> &stats, TestApplication **audio,
                           TestApplication **video)
{
    *audio = new TestApplication(&session, QStringLiteral("audio"), stats);
    *video = new TestApplication(&session, QStringLiteral("video"), stats);
    session.addContent(*audio);
    session.addContent(*video);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;

    {
        auto    stats = QSharedPointer<Stats>::create();
        Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        TestApplication *audio = nullptr, *video = nullptr;
        addInitialPair(session, stats, &audio, &video);
        QPointer<TestApplication> videoGuard(video);

        check(!session.updateFromXml(Action::SessionAccept, answer(audio, true)),
              "malformed subset session-accept was accepted");
        check(audio->state() == State::Pending, "malformed answer did not roll accepted content back");
        check(videoGuard && videoGuard->state() == State::Pending
                  && session.content(QStringLiteral("video"), Origin::Initiator) == videoGuard.data(),
              "malformed subset answer removed omitted initial content");
        check(stats->removes == 0 && stats->stops == 0, "malformed answer performed content cleanup");

        check(session.updateFromXml(Action::SessionAccept, answer(audio)), "valid subset session-accept was rejected");
        check(session.content(QStringLiteral("audio"), Origin::Initiator) == audio,
              "subset session-accept removed accepted initial content");
        check(!videoGuard && !session.content(QStringLiteral("video"), Origin::Initiator),
              "subset session-accept retained omitted initial content");
        check(stats->removes == 1 && stats->stops == 1, "omitted initial content was not cleaned up exactly once");
        pump();
        check(session.state() == State::Active && audio->state() == State::Active && stats->starts == 1,
              "accepted subset content did not start normally");
    }

    {
        auto    stats = QSharedPointer<Stats>::create();
        Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        TestApplication *audio = nullptr, *video = nullptr;
        addInitialPair(session, stats, &audio, &video);
        QPointer<TestApplication> videoGuard(video);

        check(session.updateFromXml(Action::ContentAccept, answer(audio)), "ordinary content-accept was rejected");
        check(videoGuard && videoGuard->state() == State::Pending
                  && session.content(QStringLiteral("video"), Origin::Initiator) == videoGuard.data(),
              "content-accept incorrectly removed an omitted sibling");
        check(stats->removes == 0 && stats->stops == 0, "content-accept performed subset cleanup");
    }

    qInfo("Jingle initial subset session-accept regressions passed");
}
