// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QtCrypto>
#include <iris/jingle-application.h>
#define private public
#include <iris/jingle-session.h>
#undef private
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
    explicit TestApplication(J::Session *session, J::Origin senders = J::Origin::Both,
                             const QString &contentName = QStringLiteral("audio"),
                             J::Origin creator = J::Origin::Initiator)
    {
        _pad.reset(new TestPad(session));
        _contentName = contentName;
        _creator     = creator;
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

static QDomElement payload(const J::OutgoingUpdate &update)
{
    QDomDocument doc;
    auto         jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    for (const auto &element : std::get<0>(update))
        jingle.appendChild(doc.importNode(element, true));
    doc.appendChild(jingle);
    return jingle;
}

static void acknowledge(const J::OutgoingUpdate &update, Task *result)
{
    const auto &callback = std::get<1>(update);
    check(bool(callback), "outgoing update has no ACK callback");
    callback(result);
}

static void crossedContentModify(Client &client, Task *success, Task *failure, J::Origin initiatorTarget,
                                 J::Origin responderTarget, bool responderResultFirst)
{
    J::Session initiator(client.jingleManager(), Jid(QStringLiteral("responder@example.test/device")),
                         J::Origin::Initiator);
    J::Session responder(client.jingleManager(), Jid(QStringLiteral("initiator@example.test/device")),
                         J::Origin::Responder);

    // Use a responder-created content so Session::addContent() produces the same
    // creator key on the responder that the initiator serializes in content-modify.
    auto initiatorApp = new TestApplication(&initiator, J::Origin::Both, QStringLiteral("audio"), J::Origin::Responder);
    auto responderApp = new TestApplication(&responder, J::Origin::Both, QStringLiteral("audio"), J::Origin::Responder);
    initiator.addContent(initiatorApp);
    responder.addContent(responderApp);
    initiatorApp->activate();
    responderApp->activate();

    check(initiatorApp->requestSenders(initiatorTarget), "initiator crossed direction request rejected");
    check(responderApp->requestSenders(responderTarget), "responder crossed direction request rejected");
    check(initiatorApp->evaluateOutgoingUpdate().action == J::Action::ContentModify,
          "initiator crossed request was not evaluated");
    check(responderApp->evaluateOutgoingUpdate().action == J::Action::ContentModify,
          "responder crossed request was not evaluated");
    auto initiatorUpdate = initiatorApp->takeOutgoingUpdate();
    auto responderUpdate = responderApp->takeOutgoingUpdate();

    check(initiator.shouldTieBreakIncoming(J::Action::ContentModify),
          "initiator dispatcher did not select content-modify tie-break");
    check(!responder.shouldTieBreakIncoming(J::Action::ContentModify),
          "responder incorrectly selected content-modify tie-break");
    check(!initiator.shouldTieBreakIncoming(J::Action::TransportInfo),
          "content-modify state leaked into an unrelated action");

    // JTPush rejects the responder's crossed action before updateFromXml(). The
    // responder accepts the initiator action through the real Session parser.
    check(responder.updateFromXml(J::Action::ContentModify, payload(initiatorUpdate)),
          "responder rejected initiator content-modify");
    check(responderApp->senders() == initiatorTarget, "responder did not apply initiator direction");
    check(initiatorApp->senders() == J::Origin::Both, "losing responder direction leaked into initiator state");

    if (responderResultFirst) {
        acknowledge(responderUpdate, failure);
        acknowledge(initiatorUpdate, success);
    } else {
        acknowledge(initiatorUpdate, success);
        acknowledge(responderUpdate, failure);
    }

    check(initiatorApp->senders() == initiatorTarget && responderApp->senders() == initiatorTarget,
          "crossed content-modify did not converge to initiator state");
    check(!initiator.shouldTieBreakIncoming(J::Action::ContentModify)
              && !responder.shouldTieBreakIncoming(J::Action::ContentModify),
          "crossed content-modify left a transaction marked in flight");
    check(initiatorApp->evaluateOutgoingUpdate().action == J::Action::NoAction
              && responderApp->evaluateOutgoingUpdate().action == J::Action::NoAction,
          "crossed content-modify left a redundant direction update");
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

    // The JTPush collision predicate and the existing Application ACK callbacks
    // together must converge both roles to the initiator's action, independent
    // of IQ result ordering and whether the crossed directions differ.
    crossedContentModify(client, &success, &failure, J::Origin::Initiator, J::Origin::Responder, true);
    crossedContentModify(client, &success, &failure, J::Origin::Responder, J::Origin::Responder, false);

    // The tie-break is action-level and outlives an Application removed before
    // the IQ result. The retained callback token represents the still-pending JT.
    {
        J::Session initiator(client.jingleManager(), Jid(QStringLiteral("multi@example.test/device")),
                             J::Origin::Initiator);
        auto audio = new TestApplication(&initiator, J::Origin::Both, QStringLiteral("audio"));
        auto video = new TestApplication(&initiator, J::Origin::Both, QStringLiteral("video"));
        initiator.addContent(audio);
        initiator.addContent(video);
        audio->activate();
        video->activate();
        check(audio->requestSenders(J::Origin::Initiator) && video->requestSenders(J::Origin::Responder),
              "multi-content direction request rejected");
        check(audio->evaluateOutgoingUpdate().action == J::Action::ContentModify
                  && video->evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "multi-content direction request was not evaluated");
        auto audioUpdate = audio->takeOutgoingUpdate();
        auto videoUpdate = video->takeOutgoingUpdate();
        check(initiator.shouldTieBreakIncoming(J::Action::ContentModify),
              "multi-content in-flight update did not enable tie-break");
        acknowledge(audioUpdate, &success);
        check(initiator.shouldTieBreakIncoming(J::Action::ContentModify),
              "tie-break cleared while a sibling content was still in flight");
        delete video;
        check(initiator.shouldTieBreakIncoming(J::Action::ContentModify),
              "removed application prematurely cleared action-level tie-break");
        std::get<1>(videoUpdate) = {};
        check(!initiator.shouldTieBreakIncoming(J::Action::ContentModify),
              "completed removed-content transaction left stale tie-break state");
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
