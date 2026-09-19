// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>

#include <iris/jingle-application.h>
#include <iris/xmpp_client.h>
#include <qca.h>

#include <iris/jingle-session.h>

using namespace XMPP;
using namespace XMPP::Jingle;

namespace {

void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

class TestPad final : public ApplicationManagerPad {
public:
    explicit TestPad(Session *session) : session_(session) { }

    Session *session() const override { return session_; }
    QString ns() const override { return QStringLiteral("urn:iris:test:terminate-drain"); }
    ApplicationManager *manager() const override { return nullptr; }
    QString generateContentName(Origin) override { return {}; }

private:
    Session *session_;
};

class TestApplication final : public Application {
public:
    TestApplication(Session *session, QString name, State initial)
    {
        _pad         = ApplicationManagerPad::Ptr(new TestPad(session));
        _contentName = std::move(name);
        _creator     = Origin::Initiator;
        _senders     = Origin::Both;
        _state       = initial;
    }

    void setState(State state) override
    {
        if (_state == state)
            return;
        _state = state;
        emit stateChanged(state);
    }

    const std::optional<Stanza::Error> &lastError() const override { return error_; }
    Reason lastReason() const override { return {}; }
    SetDescError setRemoteOffer(const QDomElement &) override { return Unparsed; }
    SetDescError setRemoteAnswer(const QDomElement &) override { return Unparsed; }
    QDomElement makeLocalOffer() override { return {}; }
    QDomElement makeLocalAnswer() override { return {}; }
    Update evaluateOutgoingUpdate() override { return { Action::NoAction, {} }; }
    void prepare() override { }
    void start() override { }
    void remove(Reason::Condition = Reason::Success, const QString & = {}) override { }
    void incomingRemove(const Reason &) override { }

protected:
    void prepareTransport() override { }

private:
    std::optional<Stanza::Error> error_;
};

QDomElement terminatePayload(Reason::Condition condition)
{
    QDomDocument doc;
    auto jingle = doc.createElementNS(NS, QStringLiteral("jingle"));
    jingle.appendChild(Reason(condition).toXml(&doc));
    return jingle;
}

void flushDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client client;

    // RTP-like session: no application is draining, so a normal remote hangup
    // remains immediate.
    {
        QPointer<Session> session = new Session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/a")),
                                                Origin::Initiator);
        auto *rtp = new TestApplication(session, QStringLiteral("audio"), State::Active);
        session->addContent(rtp);
        int terminated = 0;
        QObject::connect(session, &Session::terminated, &app, [&terminated]() { ++terminated; });

        check(session->updateFromXml(Action::SessionTerminate, terminatePayload(Reason::Success)),
              "normal session-terminate was rejected");
        check(session && session->state() == State::Finished && terminated == 1,
              "RTP-like session did not terminate immediately");
        flushDeletes();
        check(!session, "finished RTP-like session was not deleted");
    }

    // Mixed shape: RTP may still be Active, but an already-Finishing FT/data
    // application keeps the Session alive until its transport-specific drain
    // completes.
    {
        QPointer<Session> session = new Session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/b")),
                                                Origin::Initiator);
        auto *rtp = new TestApplication(session, QStringLiteral("audio"), State::Active);
        auto *ft  = new TestApplication(session, QStringLiteral("file"), State::Finishing);
        session->addContent(rtp);
        session->addContent(ft);
        int terminated = 0;
        QObject::connect(session, &Session::terminated, &app, [&terminated]() { ++terminated; });

        check(session->updateFromXml(Action::SessionTerminate, terminatePayload(Reason::Success)),
              "successful terminate during drain was rejected");
        check(session && session->state() == State::Finishing && terminated == 0,
              "successful terminate destroyed a draining session");
        check(ft->state() == State::Finishing && rtp->state() == State::Active,
              "deferred terminate mutated applications before drain completion");

        QCoreApplication::processEvents();
        check(session && session->state() == State::Finishing && terminated == 0,
              "event loop prematurely completed remote terminate drain");

        ft->setState(State::Finished);
        check(session && session->state() == State::Finished && terminated == 1,
              "session did not finish when the last drain completed");
        check(rtp->state() == State::Finished,
              "final session teardown did not stop the non-draining application");
        flushDeletes();
        check(!session, "drained session was not deleted");
    }

    // Cancellation/error is not peer completion. It must retain immediate
    // teardown semantics even when an application was draining.
    {
        QPointer<Session> session = new Session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/c")),
                                                Origin::Initiator);
        auto *ft = new TestApplication(session, QStringLiteral("file"), State::Finishing);
        session->addContent(ft);
        int terminated = 0;
        QObject::connect(session, &Session::terminated, &app, [&terminated]() { ++terminated; });

        check(session->updateFromXml(Action::SessionTerminate, terminatePayload(Reason::Cancel)),
              "cancel terminate was rejected");
        check(session && session->state() == State::Finished && terminated == 1,
              "cancel terminate incorrectly waited for graceful drain");
        flushDeletes();
        check(!session, "cancelled session was not deleted");
    }

    return 0;
}
