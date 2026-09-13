// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QtCrypto>
#include <iris/jingle-application.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_task.h>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class Result : public Task {
public:
    Result(Task *parent, bool ok) : Task(parent)
    {
        if (ok)
            setSuccess();
        else
            setError(500);
    }
};

class TestPad : public J::ApplicationManagerPad {
public:
    explicit TestPad(J::Session *session) : session_(session) { }

    J::Session            *session() const override { return session_; }
    QString                ns() const override { return QStringLiteral("urn:iris:test:application"); }
    J::ApplicationManager *manager() const override { return nullptr; }
    QString generateContentName(J::Origin) override { return QStringLiteral("test"); }

private:
    J::Session *session_;
};

class TestTransport : public J::Transport {
public:
    TestTransport() : Transport({}, J::Origin::Initiator) { setState(J::State::Active); }

    void prepare() override { }
    void start() override { }
    bool update(const QDomElement &) override { return true; }
    bool hasUpdates() const override { return hasUpdates_; }
    J::OutgoingTransportInfoUpdate takeOutgoingUpdate(bool) override
    {
        hasUpdates_ = false;
        return {};
    }
    bool isValid() const override { return true; }
    J::TransportFeatures features() const override { return {}; }
    J::Connection::Ptr addChannel(J::TransportFeatures, const QString &, int) override { return {}; }
    QList<J::Connection::Ptr> channels() const override { return {}; }

    bool hasUpdates_ = false;
};

class TestApplication : public J::Application {
public:
    explicit TestApplication(J::Session *session, J::Origin senders = J::Origin::Both)
    {
        _pad.reset(new TestPad(session));
        _contentName = QStringLiteral("audio");
        _creator     = J::Origin::Initiator;
        _senders     = senders;
    }

    void attachTransport()
    {
        auto transport = QSharedPointer<TestTransport>::create();
        testTransport_ = transport.data();
        _transport     = transport;
    }

    void activate()
    {
        if (!_transport)
            attachTransport();
        setState(J::State::Active);
    }

    void setTransportUpdates(bool enabled)
    {
        check(testTransport_, "test transport is missing");
        testTransport_->hasUpdates_ = enabled;
    }

    void setState(J::State state) override
    {
        if (_state == state)
            return;
        _state = state;
        emit stateChanged(state);
    }

    const std::optional<Stanza::Error> &lastError() const override { return error_; }
    J::Reason lastReason() const override { return {}; }
    SetDescError setRemoteOffer(const QDomElement &) override { return Unparsed; }
    SetDescError setRemoteAnswer(const QDomElement &) override { return Unparsed; }
    QDomElement makeLocalOffer() override { return {}; }
    QDomElement makeLocalAnswer() override { return {}; }
    bool supportsContentModify() const override { return supportsModify_; }
    void prepare() override { }
    void start() override { }
    void remove(J::Reason::Condition, const QString &) override { }
    void incomingRemove(const J::Reason &) override { }

    bool supportsModify_ = true;

protected:
    void prepareTransport() override { }

private:
    std::optional<Stanza::Error> error_;
    TestTransport               *testTransport_ = nullptr;
};

static QDomElement firstContent(const J::OutgoingUpdate &update)
{
    const auto &elements = std::get<0>(update);
    check(elements.size() == 1, "content-modify did not serialize exactly one content");
    return elements.first();
}

static QString sendersText(J::Origin senders)
{
    switch (senders) {
    case J::Origin::None:
        return QStringLiteral("none");
    case J::Origin::Both:
        return QStringLiteral("both");
    case J::Origin::Initiator:
        return QStringLiteral("initiator");
    case J::Origin::Responder:
        return QStringLiteral("responder");
    }
    return {};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    J::Session       session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")));

    Result success(client.rootTask(), true), failure(client.rootTask(), false);

    // Before the initial content stanza is consumed, changing direction only
    // changes the proposal. It must not queue a content-modify.
    {
        TestApplication initial(&session);
        int directionNotifications = 0;
        QObject::connect(&initial, &J::Application::sendersChanged, &app,
                         [&](J::Origin) { ++directionNotifications; });
        check(initial.requestSenders(J::Origin::Responder), "initial direction request rejected");
        check(initial.senders() == J::Origin::Responder, "initial direction was not changed synchronously");
        check(directionNotifications == 1, "initial direction change was not notified");
        check(initial.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "initial direction unexpectedly queued content-modify");
        check(initial.requestSenders(J::Origin::Responder), "duplicate initial direction request rejected");
        check(directionNotifications == 1, "duplicate initial direction notified again");
    }

    // All four protocol values must serialize explicitly for content-modify,
    // including the normally-defaulted value "both".
    for (const auto target :
         { J::Origin::None, J::Origin::Initiator, J::Origin::Responder, J::Origin::Both }) {
        const auto initial = target == J::Origin::Both ? J::Origin::Responder : J::Origin::Both;
        TestApplication active(&session, initial);
        active.activate();
        int directionNotifications = 0;
        QObject::connect(&active, &J::Application::sendersChanged, &app,
                         [&](J::Origin) { ++directionNotifications; });

        check(active.requestSenders(target), "active direction request rejected");
        check(active.senders() == initial, "direction changed before content-modify ACK");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "active direction did not queue content-modify");
        auto update  = active.takeOutgoingUpdate();
        auto content = firstContent(update);
        check(content.attribute(QStringLiteral("senders")) == sendersText(target),
              "content-modify serialized the wrong senders value");
        check(active.senders() == initial, "serialized direction changed before ACK");
        std::get<1>(update)(&success);
        check(active.senders() == target, "successful content-modify did not commit direction");
        check(directionNotifications == 1, "successful content-modify did not notify direction change once");
        check(active.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "successful direction request remained queued");
    }

    // A device/policy change can happen after session-initiate but before the
    // RTP application becomes Active. The intent must remain dormant and wake
    // exactly when Active is reached.
    {
        TestApplication pending(&session, J::Origin::Responder);
        pending.attachTransport();
        pending.setState(J::State::Pending);
        int updates = 0;
        QObject::connect(&pending, &J::Application::updated, &app, [&] { ++updates; });

        check(pending.requestSenders(J::Origin::Both), "pending direction request rejected");
        check(pending.senders() == J::Origin::Responder, "pending request changed negotiated direction");
        check(updates == 1, "pending direction request did not wake the session once");
        check(pending.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "pending direction leaked content-modify before Active");

        pending.setState(J::State::Connecting);
        check(updates == 1, "connecting transition woke content-modify too early");
        check(pending.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "connecting state emitted content-modify");

        pending.setState(J::State::Active);
        check(updates == 2, "queued direction was not woken on Active transition");
        check(pending.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "queued direction was lost on Active transition");
        auto update = pending.takeOutgoingUpdate();
        check(firstContent(update).attribute(QStringLiteral("senders")) == QLatin1String("both"),
              "woken direction serialized incorrectly");
        std::get<1>(update)(&success);
        check(pending.senders() == J::Origin::Both, "woken direction was not committed after ACK");
    }

    // Incoming content-modify can satisfy or conflict with a locally queued
    // pre-Active intent. A matching peer update cancels the redundant request;
    // a conflicting one keeps the local target queued until Active.
    {
        TestApplication pending(&session);
        pending.attachTransport();
        pending.setState(J::State::Pending);
        check(pending.requestSenders(J::Origin::Responder), "queued direction request rejected");
        pending.incomingContentModify(J::Origin::Responder);
        check(pending.senders() == J::Origin::Responder, "matching incoming direction not applied");
        pending.setState(J::State::Active);
        check(pending.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "matching incoming direction left a redundant content-modify");
    }
    {
        TestApplication pending(&session);
        pending.attachTransport();
        pending.setState(J::State::Pending);
        check(pending.requestSenders(J::Origin::Responder), "conflicting queued direction request rejected");
        pending.incomingContentModify(J::Origin::Initiator);
        check(pending.senders() == J::Origin::Initiator, "conflicting incoming direction not applied");
        pending.setState(J::State::Active);
        check(pending.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "conflicting incoming direction discarded local intent");
        auto update = pending.takeOutgoingUpdate();
        check(firstContent(update).attribute(QStringLiteral("senders")) == QLatin1String("responder"),
              "local intent changed after conflicting incoming direction");
        std::get<1>(update)(&success);
        check(pending.senders() == J::Origin::Responder, "local intent did not win after its ACK");
    }

    // A newer local intent supersedes an in-flight request. The ACK for the
    // first request commits what the peer acknowledged, then the latest target
    // must be sent as a second content-modify.
    {
        TestApplication active(&session);
        active.activate();

        check(active.requestSenders(J::Origin::Responder), "first superseding direction request rejected");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "first superseding direction was not queued");
        auto first = active.takeOutgoingUpdate();
        check(firstContent(first).attribute(QStringLiteral("senders")) == QLatin1String("responder"),
              "first superseding direction serialized incorrectly");

        check(active.requestSenders(J::Origin::Both), "newer direction request rejected while IQ was in flight");
        std::get<1>(first)(&success);
        check(active.senders() == J::Origin::Responder, "first ACK did not commit its negotiated direction");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "newer direction intent was lost after first ACK");
        auto second = active.takeOutgoingUpdate();
        check(firstContent(second).attribute(QStringLiteral("senders")) == QLatin1String("both"),
              "newer direction did not serialize explicit senders=both");
        std::get<1>(second)(&success);
        check(active.senders() == J::Origin::Both, "newer direction was not committed after ACK");
        check(active.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "superseded direction remained queued");
    }

    // A failed IQ must not mutate the negotiated direction or retry forever.
    {
        TestApplication active(&session);
        active.activate();
        check(active.requestSenders(J::Origin::Initiator), "failing direction request rejected locally");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "failing direction request was not queued");
        auto update = active.takeOutgoingUpdate();
        std::get<1>(update)(&failure);
        check(active.senders() == J::Origin::Both, "failed content-modify changed negotiated direction");
        check(active.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "failed content-modify was retried forever");
    }

    // Failure of an older in-flight request must retain a newer local intent.
    {
        TestApplication active(&session);
        active.activate();
        check(active.requestSenders(J::Origin::Responder), "older failing request rejected");
        auto first = active.takeOutgoingUpdate();
        check(active.requestSenders(J::Origin::Initiator), "new intent rejected while failing IQ was in flight");
        std::get<1>(first)(&failure);
        check(active.senders() == J::Origin::Both, "failed older request changed negotiated direction");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "newer intent was lost after older IQ failure");
        auto second = active.takeOutgoingUpdate();
        check(firstContent(second).attribute(QStringLiteral("senders")) == QLatin1String("initiator"),
              "newer intent serialized incorrectly after IQ failure");
        std::get<1>(second)(&success);
        check(active.senders() == J::Origin::Initiator, "newer intent did not commit after older IQ failure");
    }

    // Transport updates retain their protocol priority over content-modify.
    {
        TestApplication active(&session);
        active.activate();
        active.setTransportUpdates(true);
        check(active.requestSenders(J::Origin::Responder), "direction request with transport update rejected");
        check(active.evaluateOutgoingUpdate().action == J::Action::TransportInfo,
              "content-modify overtook transport-info");
        active.takeOutgoingUpdate();
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "direction request disappeared after transport-info");
        auto update = active.takeOutgoingUpdate();
        std::get<1>(update)(&success);
    }

    // Incoming updates are idempotent and reject unsupported/invalid cases.
    {
        TestApplication active(&session);
        active.activate();
        int directionNotifications = 0;
        QObject::connect(&active, &J::Application::sendersChanged, &app,
                         [&](J::Origin) { ++directionNotifications; });
        active.incomingContentModify(J::Origin::None);
        check(active.senders() == J::Origin::None && directionNotifications == 1,
              "incoming direction was not applied exactly once");
        active.incomingContentModify(J::Origin::None);
        check(directionNotifications == 1, "duplicate incoming direction notified again");
        active.incomingContentModify(static_cast<J::Origin>(99));
        check(active.senders() == J::Origin::None && directionNotifications == 1,
              "invalid incoming direction changed application state");
    }

    {
        TestApplication fixed(&session);
        fixed.supportsModify_ = false;
        check(!fixed.requestSenders(J::Origin::Responder), "fixed-direction application accepted content-modify");
        check(fixed.senders() == J::Origin::Both, "rejected direction request changed application state");
        fixed.incomingContentModify(J::Origin::Responder);
        check(fixed.senders() == J::Origin::Both, "fixed-direction application accepted incoming content-modify");
    }

    {
        TestApplication finished(&session);
        finished.activate();
        finished.setState(J::State::Finishing);
        check(!finished.requestSenders(J::Origin::Responder), "finishing application accepted direction request");
        check(!finished.requestSenders(static_cast<J::Origin>(99)), "invalid direction request accepted");
        check(finished.senders() == J::Origin::Both, "rejected direction request changed finishing application");
    }

    qInfo("Content-modify direction regressions passed");
}
