// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDomDocument>
#include <iris/jingle-tiebreaker.h>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static QDomElement jingle(QDomDocument &doc, const QString &name)
{
    auto el = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    el.setAttribute(QStringLiteral("name"), name);
    doc.appendChild(el);
    return el;
}

class Resolver : public J::TieBreaker::Resolver {
public:
    J::TieBreaker::Solution solution = J::TieBreaker::Solution::Continue;
    int                      resolveCalls = 0;
    int                      retryCalls = 0;
    QString                  localName;
    QString                  remoteName;
    QString                  retryRemoteName;
    J::TieBreaker::RemoteResult remoteResult = J::TieBreaker::RemoteResult::Rejected;

    J::TieBreaker::Solution resolve(const QDomElement &local, const QDomElement &remote) override
    {
        ++resolveCalls;
        localName  = local.attribute(QStringLiteral("name"));
        remoteName = remote.attribute(QStringLiteral("name"));
        return solution;
    }

    void retry(const J::TieBreaker::RetryContext &context) override
    {
        ++retryCalls;
        localName       = context.localData.attribute(QStringLiteral("name"));
        retryRemoteName = context.remoteData.attribute(QStringLiteral("name"));
        remoteResult    = context.remoteResult;
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const Stanza::Error error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::Conflict);

    // Resolvers are keyed by Action and receive the exact local/remote XML.
    {
        J::TieBreaker tieBreaker;
        Resolver      resolver;
        resolver.solution = J::TieBreaker::Solution::Postpone;
        auto registration = tieBreaker.registerResolver(J::Action::ContentModify, &resolver);
        QDomDocument localDoc, remoteDoc;
        const auto local  = jingle(localDoc, QStringLiteral("local"));
        const auto remote = jingle(remoteDoc, QStringLiteral("remote"));
        const auto tx     = tieBreaker.outgoingStarted(J::Action::ContentModify, local);

        check(tieBreaker.resolveIncoming(J::Action::TransportInfo, remote).solution
        == J::TieBreaker::Solution::Continue,
    "unrelated action reached a content-modify resolver");
        const auto resolution = tieBreaker.resolveIncoming(J::Action::ContentModify, remote);
        check(resolution.solution == J::TieBreaker::Solution::Postpone && resolution.id != 0,
    "postpone resolver was not armed");
        check(resolver.resolveCalls == 1 && resolver.localName == QLatin1String("local")
        && resolver.remoteName == QLatin1String("remote"),
    "resolver did not receive local/remote Jingle data");
        check(registration.isPostponed(), "registration did not expose postponed state");

        tieBreaker.incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Applied);
        tieBreaker.outgoingFinished(tx, error);
        check(resolver.retryCalls == 0, "retry ran before owner IQ callback finished");
        // The transaction is no longer in flight before that callback.
        check(tieBreaker.resolveIncoming(J::Action::ContentModify, remote).solution
        == J::TieBreaker::Solution::Continue,
    "completed IQ remained visible as a simultaneous action");
        tieBreaker.outgoingCallbacksFinished(tx);
        check(resolver.retryCalls == 1 && resolver.remoteResult == J::TieBreaker::RemoteResult::Applied
                  && resolver.retryRemoteName == QLatin1String("remote"),
    "failed postponed IQ did not retry with full collision context after owner callback");
        check(!registration.isPostponed(), "retry left registration postponed");
    }

    // Success means the peer accepted our local proposal: no retry is necessary.
    {
        J::TieBreaker tieBreaker;
        Resolver      resolver;
        resolver.solution = J::TieBreaker::Solution::Postpone;
        auto registration = tieBreaker.registerResolver(J::Action::ContentModify, &resolver);
        QDomDocument localDoc, remoteDoc;
        const auto tx = tieBreaker.outgoingStarted(J::Action::ContentModify,
                                          jingle(localDoc, QStringLiteral("accepted")));
        const auto resolution
  = tieBreaker.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc, QStringLiteral("remote")));
        tieBreaker.outgoingFinished(tx, std::nullopt);
        tieBreaker.outgoingCallbacksFinished(tx);
        tieBreaker.incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Rejected);
        check(resolver.retryCalls == 0 && !registration.isPostponed(),
    "accepted local proposal incorrectly retried postponed intent");
    }

    // Break dominates earlier Postpone decisions and must not arm retry state.
    {
        J::TieBreaker tieBreaker;
        Resolver      postpone, breaker;
        postpone.solution = J::TieBreaker::Solution::Postpone;
        breaker.solution  = J::TieBreaker::Solution::Break;
        auto postponeRegistration = tieBreaker.registerResolver(J::Action::ContentModify, &postpone);
        auto breakRegistration    = tieBreaker.registerResolver(J::Action::ContentModify, &breaker);
        QDomDocument localDoc, remoteDoc;
        tieBreaker.outgoingStarted(J::Action::ContentModify, jingle(localDoc, QStringLiteral("local")));
        const auto result
  = tieBreaker.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc, QStringLiteral("remote")));
        check(result.solution == J::TieBreaker::Solution::Break && result.id == 0,
    "Break did not dominate Postpone");
        check(postpone.resolveCalls == 1 && breaker.resolveCalls == 1,
    "Break short-circuited another resolver instead of evaluating the full action");
        check(!postponeRegistration.isPostponed() && !breakRegistration.isPostponed(),
    "Break armed postponed retry state");
    }

    // Remote processing may finish after our error. retry waits for both facts.
    {
        J::TieBreaker tieBreaker;
        Resolver      resolver;
        resolver.solution = J::TieBreaker::Solution::Postpone;
        auto registration = tieBreaker.registerResolver(J::Action::ContentModify, &resolver);
        QDomDocument localDoc, remoteDoc;
        const auto tx = tieBreaker.outgoingStarted(J::Action::ContentModify, jingle(localDoc, QStringLiteral("local")));
        const auto resolution
  = tieBreaker.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc, QStringLiteral("remote")));
        tieBreaker.outgoingFinished(tx, error);
        tieBreaker.outgoingCallbacksFinished(tx);
        check(resolver.retryCalls == 0, "retry ran before remote processing completed");
        tieBreaker.incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Rejected);
        check(resolver.retryCalls == 1 && resolver.remoteResult == J::TieBreaker::RemoteResult::Rejected,
    "remote rejection was not delivered to retry");
    }

    // Destroying a registration or clearing the Session state cancels callbacks.
    {
        J::TieBreaker tieBreaker;
        Resolver      resolver;
        resolver.solution = J::TieBreaker::Solution::Postpone;
        J::TieBreaker::Registration registration
  = tieBreaker.registerResolver(J::Action::ContentModify, &resolver);
        QDomDocument localDoc, remoteDoc;
        const auto tx = tieBreaker.outgoingStarted(J::Action::ContentModify, jingle(localDoc, QStringLiteral("local")));
        const auto resolution
  = tieBreaker.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc, QStringLiteral("remote")));
        check(registration.isPostponed(), "teardown test did not arm postpone");
        registration = {};
        tieBreaker.incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Applied);
        tieBreaker.outgoingFinished(tx, error);
        tieBreaker.outgoingCallbacksFinished(tx);
        check(resolver.retryCalls == 0, "destroyed resolver registration was called");

        Resolver second;
        second.solution = J::TieBreaker::Solution::Postpone;
        auto secondRegistration = tieBreaker.registerResolver(J::Action::ContentModify, &second);
        QDomDocument localDoc2, remoteDoc2;
        const auto tx2 = tieBreaker.outgoingStarted(J::Action::ContentModify, jingle(localDoc2, QStringLiteral("two")));
        const auto resolution2
  = tieBreaker.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc2, QStringLiteral("remote-two")));
        check(secondRegistration.isPostponed(), "clear test did not arm postpone");
        tieBreaker.clear();
        tieBreaker.incomingFinished(resolution2.id, J::TieBreaker::RemoteResult::Applied);
        tieBreaker.outgoingFinished(tx2, error);
        tieBreaker.outgoingCallbacksFinished(tx2);
        check(second.retryCalls == 0 && !secondRegistration.isPostponed(),
    "clear left stale postponed callbacks");
    }

    return 0;
}
