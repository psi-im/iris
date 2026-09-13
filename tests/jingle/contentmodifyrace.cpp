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
    QString generateContentName(J::Origin) override { return QStringLiteral("audio"); }

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
    bool supportsContentModify() const override { return true; }
    void prepare() override { }
    void start() override { }
    void remove(J::Reason::Condition, const QString &) override { }
    void incomingRemove(const J::Reason &) override { }

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

static void acknowledge(const J::OutgoingUpdate &update, Task *result)
{
    const auto &callback = std::get<1>(update);
    check(bool(callback), "outgoing update has no ACK callback");
    callback(result);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    J::Session       session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")));
    Result           success(client.rootTask(), true), failure(client.rootTask(), false);

    // Hotplug after session-initiate but before RTP reaches Active must retain
    // the desired direction without sending content-modify prematurely.
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
        check(updates == 1, "Connecting woke content-modify too early");
        check(pending.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "Connecting emitted content-modify");

        pending.setState(J::State::Active);
        check(updates == 2, "queued direction was not woken on Active transition");
        check(pending.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "queued direction was lost on Active transition");
        auto update = pending.takeOutgoingUpdate();
        check(firstContent(update).attribute(QStringLiteral("senders")) == QLatin1String("both"),
              "woken direction serialized incorrectly");
        acknowledge(update, &success);
        check(pending.senders() == J::Origin::Both, "woken direction was not committed after ACK");
    }

    // A matching peer update satisfies a queued local target. A conflicting
    // peer update changes current negotiation but must not discard local intent.
    {
        TestApplication pending(&session);
        pending.attachTransport();
        pending.setState(J::State::Pending);
        check(pending.requestSenders(J::Origin::Responder), "queued direction request rejected");
        pending.incomingContentModify(J::Origin::Responder);
        check(pending.senders() == J::Origin::Responder, "matching incoming direction not applied");
        pending.setState(J::State::Active);
        check(pending.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "matching incoming direction left redundant content-modify");
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
        acknowledge(update, &success);
        check(pending.senders() == J::Origin::Responder, "local intent did not commit after ACK");
    }

    // If an older request fails while a newer policy target is queued, only the
    // old request is discarded. The newer target remains actionable.
    {
        TestApplication active(&session);
        active.activate();
        check(active.requestSenders(J::Origin::Responder), "older failing request rejected");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "older failing request was not evaluated");
        auto first = active.takeOutgoingUpdate();
        check(firstContent(first).attribute(QStringLiteral("senders")) == QLatin1String("responder"),
              "older failing request serialized incorrectly");
        check(active.requestSenders(J::Origin::Initiator), "new intent rejected while IQ was in flight");
        acknowledge(first, &failure);
        check(active.senders() == J::Origin::Both, "failed older request changed negotiated direction");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "newer intent was lost after older IQ failure");
        auto second = active.takeOutgoingUpdate();
        check(firstContent(second).attribute(QStringLiteral("senders")) == QLatin1String("initiator"),
              "newer intent serialized incorrectly after IQ failure");
        acknowledge(second, &success);
        check(active.senders() == J::Origin::Initiator, "newer intent did not commit after older IQ failure");
    }

    // Transport signaling has protocol priority and must not consume the queued
    // direction change.
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
        acknowledge(update, &success);
        check(active.senders() == J::Origin::Responder, "direction did not commit after transport-info");
    }

    // Incoming changes are idempotent and invalid enum values are ignored.
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

    qInfo("Content-modify race regressions passed");
}
