#include "../../src/xmpp/xmpp-im/jingle-ice-group_p.h"

#include <QCoreApplication>
#include <QPointer>

using namespace XMPP::Jingle;
using namespace XMPP::Jingle::ICE;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static GroupNegotiation::TransportParameters transportParameters()
{
    return { QStringLiteral("ufrag"), QStringLiteral("password"), QStringLiteral("sha-256"),
             QByteArray::fromHex("01020304"), QStringLiteral("actpass") };
}

static GroupNegotiation::Member member(const char *name)
{
    return { ContentKey { QString::fromLatin1(name), Origin::Initiator },
             QStringLiteral("urn:xmpp:jingle:transports:ice:0"), true, transportParameters() };
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QList<GroupNegotiation::Member> members { member("audio"), member("video"), member("screen") };
    const QList<ContentGroup> offer { { QStringLiteral("BUNDLE"), { QStringLiteral("audio"), QStringLiteral("video") } } };
    const QList<ContentGroup> answer { { QStringLiteral("BUNDLE"), { QStringLiteral("video"), QStringLiteral("audio") } } };

    auto plan = GroupNegotiation::initialPlan(members, offer, answer);
    check(plan && plan->readyToCommit(), "valid group plan was not committable");

    ConnectionRegistry registry;
    auto transaction = ConnectionGroupTransaction::commit(*plan, registry);
    check(transaction && transaction->size() == 3, "valid group plan did not commit all memberships");

    auto shared = transaction->connectionFor(members.at(0).content);
    auto video  = transaction->connectionFor(members.at(1).content);
    auto screen = transaction->connectionFor(members.at(2).content);
    check(shared && shared == video && shared != screen, "committed membership topology did not match the plan");
    check(transaction->associationIdFor(members.at(0).content)
              == transaction->associationIdFor(members.at(1).content)
              && transaction->associationIdFor(members.at(0).content)
                  != transaction->associationIdFor(members.at(2).content),
          "live association ids did not match planned grouping");

    QPointer<IceConnection> sharedGuard(shared);
    QPointer<IceConnection> screenGuard(screen);
    check(transaction->release(members.at(1).content), "owner membership could not be released");
    check(sharedGuard && transaction->connectionFor(members.at(0).content) == sharedGuard,
          "removing the negotiated owner destroyed a surviving member");
    check(transaction->release(members.at(0).content), "surviving shared membership could not be released");
    check(!sharedGuard && screenGuard, "last shared release affected the independent association");
    check(transaction->release(members.at(2).content), "independent membership could not be released");
    check(!screenGuard && registry.liveAssociationCount() == 0, "final release retained a network association");

    auto refusalPlan = GroupNegotiation::initialPlan(members, offer, {});
    check(refusalPlan && refusalPlan->readyToCommit(), "BUNDLE refusal did not produce a committable fallback plan");
    auto refusal = ConnectionGroupTransaction::commit(*refusalPlan, registry);
    check(refusal && refusal->connectionFor(members.at(0).content) != refusal->connectionFor(members.at(1).content)
              && refusal->connectionFor(members.at(0).content) != refusal->connectionFor(members.at(2).content)
              && refusal->connectionFor(members.at(1).content) != refusal->connectionFor(members.at(2).content),
          "BUNDLE refusal reused an unnegotiated shared association");
    refusal.reset();
    check(registry.liveAssociationCount() == 0, "fallback transaction leaked associations");

    auto preflightMembers = members;
    preflightMembers[0].transportParameters.reset();
    preflightMembers[1].transportParameters.reset();
    auto preflightPlan = GroupNegotiation::initialPlan(preflightMembers, offer, answer);
    check(preflightPlan && !preflightPlan->readyToCommit(), "parameter-less plan unexpectedly became committable");
    check(!ConnectionGroupTransaction::commit(*preflightPlan, registry) && registry.liveAssociationCount() == 0,
          "preflight-only plan mutated the live registry");

    // Force failure on the last planned content. Audio/video associations are
    // created first, so a non-transactional implementation would leak them here.
    auto existingScreen = registry.create(members.at(2).content);
    check(existingScreen && registry.liveAssociationCount() == 1, "failed to seed rollback regression");
    check(!ConnectionGroupTransaction::commit(*plan, registry), "duplicate final member did not fail the commit");
    check(registry.liveAssociationCount() == 1 && existingScreen.membershipCount() == 1,
          "failed commit left a partially applied group");
    auto postRollbackAudio = registry.create(members.at(0).content);
    check(bool(postRollbackAudio), "rollback retained an earlier temporary audio membership");
    postRollbackAudio.reset();
    existingScreen.reset();
    check(registry.liveAssociationCount() == 0, "rollback regression leaked the seeded association");

    qInfo("Jingle ICE group commit regressions passed");
}
