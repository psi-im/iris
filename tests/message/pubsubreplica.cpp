/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <iris/xmpp_pubsubreplica.h>

#include <QCoreApplication>
#include <QString>

using Replica = XMPP::PubSubReplica<QString>;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static void snapshotAndEvents()
{
    Replica replica;
    const auto epoch = replica.beginLoad();
    check(epoch != 0 && replica.state() == Replica::State::Loading, "Load did not start");
    replica.published(QStringLiteral("new"), QStringLiteral("push-new"));
    replica.retracted(QStringLiteral("deleted"));
    replica.published(QStringLiteral("same"), QStringLiteral("newer"));
    check(replica.finishLoad(epoch, {
        { QStringLiteral("deleted"), QStringLiteral("old") },
        { QStringLiteral("same"), QStringLiteral("stale") },
        { QStringLiteral("untouched"), QStringLiteral("keep") }
    }), "Load failed");
    check(replica.items().size() == 3, "Snapshot+push cardinality incorrect");
    check(replica.items().value(QStringLiteral("same")) == QStringLiteral("newer"),
          "Old IQ replaced a newer PEP event");
    check(!replica.items().contains(QStringLiteral("deleted")), "Buffered retraction was lost");
    check(replica.items().value(QStringLiteral("new")) == QStringLiteral("push-new"),
          "Buffered publication was lost");
}

static void queuedEditsAndStaleCallbacks()
{
    Replica replica;
    check(replica.finishLoad(replica.beginLoad(), {
        { QStringLiteral("roomA"), QStringLiteral("old") }
    }), "Initial load failed");
    replica.put(QStringLiteral("roomA"), QStringLiteral("new-local"));
    auto write = replica.beginWrite();
    check(write.isValid() && replica.isWriting() && write.changes.size() == 1,
          "Local write was not scheduled");
    check(!replica.beginWrite().isValid(), "Overlapping batch was allowed");
    replica.put(QStringLiteral("roomC"), QStringLiteral("queued"));
    replica.put(QStringLiteral("roomC"), QStringLiteral("latest"));
    replica.published(QStringLiteral("roomB"), QStringLiteral("other-device"));
    replica.published(QStringLiteral("roomA"), QStringLiteral("newer-push"));
    check(replica.finishWrite(write, true), "Successful batch not confirmed");
    check(replica.items().value(QStringLiteral("roomA")) == QStringLiteral("newer-push"),
          "Late callback overwrote newer remote state");
    check(replica.items().value(QStringLiteral("roomB")) == QStringLiteral("other-device"),
          "Unrelated remote publication was dropped");
    auto second = replica.beginWrite();
    check(second.changes.size() == 1 && second.changes.value(QStringLiteral("roomC"))
              == Replica::Value(QStringLiteral("latest")),
          "Queued changes were not coalesced");
    check(!replica.finishWrite(write, true), "Duplicate callback confirmed a different batch");
    check(replica.finishWrite(second, true), "Queued second batch failed");
    check(replica.items().value(QStringLiteral("roomC")) == QStringLiteral("latest"),
          "Queued edit missing after acknowledgement");
}

static void failedWritesAndReconnect()
{
    Replica replica;
    check(replica.finishLoad(replica.beginLoad(), {}), "Could not load empty node");
    replica.put(QStringLiteral("one"), QStringLiteral("first"));
    replica.remove(QStringLiteral("two"));
    auto batch = replica.beginWrite();
    replica.put(QStringLiteral("one"), QStringLiteral("new-intent"));
    check(replica.finishWrite(batch, false), "Failed batch not consumed");
    check(replica.state() == Replica::State::ResyncRequired, "Failed write did not require reload");
    check(!replica.beginWrite().isValid(), "Write issued before authoritative reload");
    const auto epoch = replica.beginLoad();
    check(replica.finishLoad(epoch, { { QStringLiteral("one"), QStringLiteral("remote") } }),
          "Reload after failure failed");
    auto retry = replica.beginWrite();
    check(retry.changes.size() == 2, "Unconfirmed edits were lost");
    check(retry.changes.value(QStringLiteral("one")) == Replica::Value(QStringLiteral("new-intent")),
          "New user edit did not supersede failed old edit");
    check(!retry.changes.value(QStringLiteral("two")).has_value(), "Deletion intent was lost");
    replica.reset();
    check(!replica.finishWrite(retry, true), "Callback from prior session mutated replica");
    check(!replica.finishLoad(epoch, {}), "Old snapshot applied after reset");
    check(replica.state() == Replica::State::Idle && replica.items().isEmpty(),
          "Account reset leaked old server state");
}

static void invalidationAndLoadTokens()
{
    Replica replica;
    const auto firstLoad = replica.beginLoad();
    replica.invalidate();
    const auto secondLoad = replica.beginLoad();
    check(secondLoad != firstLoad, "Separate fetches reused the same token");
    check(!replica.finishLoad(firstLoad, {{QStringLiteral("old"), QStringLiteral("bad")}}),
          "Stale same-session fetch overwrote a newer fetch");
    check(replica.finishLoad(secondLoad, {}), "Fresh fetch result was not accepted");

    replica.put(QStringLiteral("a"), QStringLiteral("intended"));
    const auto firstWrite = replica.beginWrite();
    replica.put(QStringLiteral("b"), QStringLiteral("queued"));
    replica.invalidate(); // e.g. node deletion while a batch is in flight
    check(replica.state() == Replica::State::ResyncRequired, "Invalidation failed");
    check(!replica.finishWrite(firstWrite, true), "Invalidated write was accepted");
    const auto refresh = replica.beginLoad();
    check(replica.finishLoad(refresh, {}), "Load after invalidation failed");
    const auto retry = replica.beginWrite();
    check(retry.changes.size() == 2, "Node invalidation lost in-flight/queued intents");
    check(retry.changes.value(QStringLiteral("a")) == Replica::Value(QStringLiteral("intended")),
          "Original unconfirmed intent was lost");
    check(retry.changes.value(QStringLiteral("b")) == Replica::Value(QStringLiteral("queued")),
          "Newer queued intent was lost");
    check(replica.finishWrite(retry, true), "Retry after invalidation failed");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    snapshotAndEvents();
    queuedEditsAndStaleCallbacks();
    failedWritesAndReconnect();
    invalidationAndLoadTokens();
    return 0;
}
