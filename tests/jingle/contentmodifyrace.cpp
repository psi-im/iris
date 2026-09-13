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

// Outgoing update elements are built directly in the Client document. A real
// stanza parser puts an unprefixed <content/> under the Jingle default namespace
// into urn:xmpp:jingle:1. Recreate that parsed shape here instead of merely
// importing the DOM node, which would preserve its empty namespaceURI().
static QDomElement payload(const J::OutgoingUpdate &update)
{
    QDomDocument doc;
    auto         jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    for (const auto &element : std::get<0>(update)) {
        auto content = doc.createElementNS(J::NS, QStringLiteral("content"));
        for (const auto &name : { QStringLiteral("creator"), QStringLiteral("name"), QStringLiteral("senders") }) {
            if (element.hasAttribute(name))
                content.setAttribute(name, element.attribute(name));
        }
        jingle.appendChild(content);
    }
    doc.appendChild(jingle);
    return jingle;
}

static void acknowledge(const J::OutgoingUpdate &update, Task *result)
{
    const auto &callback = std::get<1>(update);
    check(bool(callback), "outgoing update has no ACK callback");
    callback(result);
}

static void testGenericResolver()
{
    struct Context {
        int kind = 0;
    };
    enum class Decision { Pass, Reject };
    enum class Event { Rejected, Wake };
    using Resolver = J::TieBreakResolver<Context, Decision, Event>;

    Resolver resolver;
    bool     retryReady    = false;
    int      immediate     = 0;
    int      advances      = 0;
    int      conflictCalls = 0;

    const auto registration = resolver.registerResolver(Resolver::Callbacks {
        [count = 0, &conflictCalls](const Context &context) mutable {
            conflictCalls = ++count;
            return context.kind == 7;
        },
        [&](const Context &) {
            Resolver::Plan plan;
            plan.decision  = Decision::Reject;
            plan.immediate = [&]() { ++immediate; };
            plan.advance   = [phase = 0, &advances, &retryReady](const Event &event) mutable {
                advances = ++phase;
                return event == Event::Wake && retryReady ? Resolver::ResolutionState::Finished
                                                          : Resolver::ResolutionState::Waiting;
            };
            return plan;
        } });
    check(registration != 0, "generic tie-break resolver registration failed");

    const auto first = resolver.resolve(Context { 0 });
    check(!first.handled && conflictCalls == 1, "generic resolver did not preserve first callback invocation");

    const auto decision = resolver.resolve(Context { 7 });
    check(decision.handled && decision.decision == Decision::Reject && conflictCalls == 2,
          "generic resolver ignored registered callback or lost mutable callback state");
    check(immediate == 1, "generic resolver did not run immediate callback exactly once");
    check(decision.resolution != 0 && resolver.hasResolution(decision.resolution),
          "generic resolver did not create per-collision state machine");

    resolver.notify(decision.resolution, Event::Rejected);
    check(resolver.hasResolution(decision.resolution) && advances == 1,
          "resolution finished before retry condition became true");
    retryReady = true;
    resolver.notify(decision.resolution, Event::Wake);
    check(!resolver.hasResolution(decision.resolution) && advances == 2,
          "resolution did not preserve mutable state across events");

    resolver.unregisterResolver(registration);
    check(!resolver.resolve(Context { 7 }).handled, "unregistered resolver still handled a collision");
}

static void crossedContentModify(Client &client, Task *success, Task *failure, J::Origin initiatorTarget,
                                 J::Origin responderTarget, bool responderResultFirst)
{
    J::Session initiator(client.jingleManager(), Jid(QStringLiteral("responder@example.test/device")),
                         J::Origin::Initiator);
    J::Session responder(client.jingleManager(), Jid(QStringLiteral("initiator@example.test/device")),
                         J::Origin::Responder);

    // Use a responder-created content so both sessions address the same
    // (creator,name) key while exercising opposite Jingle roles.
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
    initiator.outgoingActionStarted(J::Action::ContentModify);
    responder.outgoingActionStarted(J::Action::ContentModify);

    check(initiator.shouldTieBreakIncoming(J::Action::ContentModify),
          "initiator dispatcher did not reject crossed content-modify");
    check(!responder.shouldTieBreakIncoming(J::Action::ContentModify),
          "responder incorrectly rejected initiator content-modify");
    check(!initiator.shouldTieBreakIncoming(J::Action::TransportInfo),
          "content-modify collision leaked into unrelated action");

    check(responder.updateFromXml(J::Action::ContentModify, payload(initiatorUpdate)),
          "responder rejected initiator content-modify");
    check(responderApp->senders() == initiatorTarget, "responder did not apply initiator direction");
    check(initiatorApp->senders() == J::Origin::Both, "losing responder direction leaked into initiator state");

    if (responderResultFirst) {
        responder.outgoingActionFinished(J::Action::ContentModify);
        acknowledge(responderUpdate, failure);
        initiator.outgoingActionFinished(J::Action::ContentModify);
        acknowledge(initiatorUpdate, success);
    } else {
        initiator.outgoingActionFinished(J::Action::ContentModify);
        acknowledge(initiatorUpdate, success);
        responder.outgoingActionFinished(J::Action::ContentModify);
        acknowledge(responderUpdate, failure);
    }

    check(initiatorApp->senders() == initiatorTarget && responderApp->senders() == initiatorTarget,
          "crossed content-modify did not converge to initiator state");
    check(!initiator.shouldTieBreakIncoming(J::Action::ContentModify)
              && !responder.shouldTieBreakIncoming(J::Action::ContentModify),
          "crossed content-modify left collision state in flight");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    Result           success(client.rootTask(), true), failure(client.rootTask(), false);

    testGenericResolver();

    // A direction intent queued before Active wakes exactly when it becomes
    // legal to send content-modify.
    {
        J::Session      session(client.jingleManager(), Jid(QStringLiteral("pending@example.test/device")));
        TestApplication pending(&session, J::Origin::Responder);
        pending.attachTransport();
        pending.setState(J::State::Pending);
        int updates = 0;
        QObject::connect(&pending, &J::Application::updated, &app, [&]() { ++updates; });

        check(pending.requestSenders(J::Origin::Both), "pending direction request rejected");
        check(pending.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "pending direction leaked content-modify before Active");
        pending.setState(J::State::Connecting);
        check(pending.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "Connecting emitted content-modify");
        pending.setState(J::State::Active);
        check(updates == 2 && pending.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "queued direction was not woken on Active transition");
        auto update = pending.takeOutgoingUpdate();
        check(firstContent(update).attribute(QStringLiteral("senders")) == QLatin1String("both"),
              "woken direction serialized incorrectly");
        acknowledge(update, &success);
        check(pending.senders() == J::Origin::Both, "woken direction was not committed after ACK");
    }

    // A newer intent must survive failure of the older in-flight request.
    {
        J::Session      session(client.jingleManager(), Jid(QStringLiteral("supersede@example.test/device")));
        TestApplication active(&session);
        active.activate();
        check(active.requestSenders(J::Origin::Responder), "older failing request rejected");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "older failing request was not evaluated");
        auto first = active.takeOutgoingUpdate();
        check(active.requestSenders(J::Origin::Initiator), "new intent rejected while IQ was in flight");
        acknowledge(first, &failure);
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "newer intent was lost after older IQ failure");
        auto second = active.takeOutgoingUpdate();
        check(firstContent(second).attribute(QStringLiteral("senders")) == QLatin1String("initiator"),
              "newer intent serialized incorrectly after IQ failure");
        acknowledge(second, &success);
        check(active.senders() == J::Origin::Initiator, "newer intent did not commit after older IQ failure");
    }

    // Both direction values and IQ result orderings exercise the same generic
    // collision resolver. The initiator action is the immediate protocol winner.
    crossedContentModify(client, &success, &failure, J::Origin::Initiator, J::Origin::Responder, true);
    crossedContentModify(client, &success, &failure, J::Origin::Responder, J::Origin::Responder, false);

    // Collision lifetime must survive removal of one Application while another
    // content-modify callback representing the same action is still pending.
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
        audio->evaluateOutgoingUpdate();
        video->evaluateOutgoingUpdate();
        auto audioUpdate = audio->takeOutgoingUpdate();
        auto videoUpdate = video->takeOutgoingUpdate();
        initiator.outgoingActionStarted(J::Action::ContentModify);
        check(initiator.shouldTieBreakIncoming(J::Action::ContentModify),
              "multi-content collision did not enable resolver");
        delete video;
        std::get<1>(videoUpdate) = {};
        check(initiator.shouldTieBreakIncoming(J::Action::ContentModify),
              "removed Application prematurely cleared the still-pending IQ collision");
        initiator.outgoingActionFinished(J::Action::ContentModify);
        check(!initiator.shouldTieBreakIncoming(J::Action::ContentModify),
              "completed multi-content IQ left stale collision state");
        acknowledge(audioUpdate, &success);
    }

    // Transport signaling keeps its existing priority over a queued direction
    // update; introducing the generic resolver must not change that scheduler.
    {
        J::Session      session(client.jingleManager(), Jid(QStringLiteral("transport@example.test/device")));
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
    }

    qInfo("Generic tie-break and content-modify race regressions passed");
}
