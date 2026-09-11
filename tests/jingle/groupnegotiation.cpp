#include "../../src/xmpp/xmpp-im/jingle-group-negotiation_p.h"

#include <QCoreApplication>

using namespace XMPP::Jingle;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static GroupNegotiation::Member member(const char *name, const char *transport = "urn:xmpp:jingle:transports:ice:0",
                                       bool shareable = true, Origin creator = Origin::Initiator)
{
    return GroupNegotiation::Member { ContentKey { QString::fromLatin1(name), creator }, QString::fromLatin1(transport),
                                      shareable };
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QList<GroupNegotiation::Member> members { member("audio"), member("video"), member("screen") };
    const QList<ContentGroup> offer { { QStringLiteral("BUNDLE"), { QStringLiteral("audio"), QStringLiteral("video") } } };

    GroupNegotiation::Error error = GroupNegotiation::Error::None;
    auto plan = GroupNegotiation::initialPlan(
        members, offer,
        { { QStringLiteral("BUNDLE"), { QStringLiteral("video"), QStringLiteral("audio") } } }, &error);
    check(plan.has_value() && error == GroupNegotiation::Error::None, "valid BUNDLE plan rejected");
    check(plan->associations().size() == 2, "shared and independent members did not form two associations");
    const auto &shared = plan->associations().at(0);
    check(shared.bundled && shared.owner == members.at(1).content, "answer order did not select the BUNDLE owner");
    check(shared.members == QList<ContentKey>({ members.at(1).content, members.at(0).content }),
          "BUNDLE member order was not preserved");
    check(shared.transportNamespace == members.at(0).transportNamespace, "shared transport namespace was lost");
    const auto &independent = plan->associations().at(1);
    check(!independent.bundled && independent.members == QList<ContentKey>({ members.at(2).content }),
          "ungrouped content was not kept independent");
    check(plan->associationFor(members.at(0).content) == shared.id
              && plan->associationFor(members.at(1).content) == shared.id
              && plan->associationFor(members.at(2).content) == independent.id,
          "membership lookup returned the wrong association");
    check(plan->actions().size() == 5
              && plan->actions().at(0).kind == GroupPlan::ActionKind::CreateAssociation
              && plan->actions().at(1).kind == GroupPlan::ActionKind::AttachMember
              && plan->actions().at(2).content == members.at(0).content,
          "transaction actions were not ordered by association and negotiated membership");

    auto refusal = GroupNegotiation::initialPlan(members, offer, {}, &error);
    check(refusal && refusal->associations().size() == 3, "BUNDLE refusal did not preserve independent transports");
    for (const auto &association : refusal->associations())
        check(!association.bundled && association.members.size() == 1, "refused BUNDLE still shared an association");

    auto subset = GroupNegotiation::initialPlan(
        members, offer, { { QStringLiteral("BUNDLE"), { QStringLiteral("video") } } }, &error);
    check(subset && subset->associations().size() == 3 && subset->associations().first().bundled
              && subset->associations().first().owner == members.at(1).content,
          "valid subset answer was not represented without merging rejected members");

    const QList<ContentGroup> twoOffers {
        { QStringLiteral("BUNDLE"), { QStringLiteral("audio"), QStringLiteral("video") } },
        { QStringLiteral("BUNDLE"), { QStringLiteral("screen") } }
    };
    check(!GroupNegotiation::initialPlan(
              members, twoOffers,
              { { QStringLiteral("BUNDLE"), { QStringLiteral("audio"), QStringLiteral("screen") } } }, &error)
              && error == GroupNegotiation::Error::InvalidAnswer,
          "answer merged distinct offered groups");
    check(!GroupNegotiation::initialPlan(
              members, offer,
              { { QStringLiteral("BUNDLE"), { QStringLiteral("audio") } },
                { QStringLiteral("BUNDLE"), { QStringLiteral("video") } } },
              &error)
              && error == GroupNegotiation::Error::InvalidAnswer,
          "answer split one offered group into multiple associations");
    check(!GroupNegotiation::initialPlan(
              members, offer,
              { { QStringLiteral("BUNDLE"), { QStringLiteral("audio"), QStringLiteral("missing") } } }, &error)
              && error == GroupNegotiation::Error::InvalidAnswer,
          "answer added an unoffered content");

    auto mixedMembers = members;
    mixedMembers[1].transportNamespace = QStringLiteral("urn:xmpp:jingle:transports:ice-udp:1");
    check(!GroupNegotiation::initialPlan(
              mixedMembers, offer,
              { { QStringLiteral("BUNDLE"), { QStringLiteral("audio"), QStringLiteral("video") } } }, &error)
              && error == GroupNegotiation::Error::ConflictingTransport,
          "mixed wire transport namespaces shared one association");

    auto unsupportedMembers = members;
    unsupportedMembers[1].shareable = false;
    check(!GroupNegotiation::initialPlan(
              unsupportedMembers, offer,
              { { QStringLiteral("BUNDLE"), { QStringLiteral("audio"), QStringLiteral("video") } } }, &error)
              && error == GroupNegotiation::Error::UnsupportedSharedTransport,
          "non-shareable transport entered a shared association");

    auto ambiguousMembers = members;
    ambiguousMembers.append(member("audio", "urn:xmpp:jingle:transports:ice:0", true, Origin::Responder));
    check(!GroupNegotiation::initialPlan(ambiguousMembers, offer, {}, &error)
              && error == GroupNegotiation::Error::AmbiguousContent,
          "group reference silently selected one creator for an ambiguous content name");

    auto duplicateMembers = members;
    duplicateMembers.append(members.first());
    check(!GroupNegotiation::initialPlan(duplicateMembers, offer, {}, &error)
              && error == GroupNegotiation::Error::DuplicateContent,
          "duplicate internal content identity accepted");

    check(!GroupNegotiation::initialPlan(
              members, { { QStringLiteral("BUNDLE"), { QStringLiteral("audio"), QStringLiteral("audio") } } }, {},
              &error)
              && error == GroupNegotiation::Error::InvalidGroup,
          "duplicate member in local offer accepted");

    qInfo("Jingle group negotiation regressions passed");
}
