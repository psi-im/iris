// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QEventLoop>
#include <QDebug>
#include <iris/jingle-rtp.h>

namespace R = XMPP::Jingle::RTP;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static void pump()
{
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
}

static R::Description description(const QString &media)
{
    R::Description result;
    result.media   = media;
    result.rtcpMux = true;
    R::PayloadType payload;
    payload.id        = media == QLatin1String("audio") ? 111 : 96;
    payload.name      = media == QLatin1String("audio") ? QStringLiteral("opus") : QStringLiteral("VP8");
    payload.clockrate = media == QLatin1String("audio") ? 48000 : 90000;
    payload.channels  = media == QLatin1String("audio") ? 2 : 1;
    result.payloads.append(payload);
    return result;
}

class Endpoint : public R::MediaEndpoint {
public:
    explicit Endpoint(QString media) : media(std::move(media)) { }
    R::Description localOffer() const override { return description(media); }
    std::optional<R::Description> makeAnswer(const R::Description &offer) const override { return offer; }
    bool acceptsAnswer(const R::Description &, const R::Description &) const override { return true; }
    bool configure(const R::Description &local, const R::Description &remote) override
    {
        ++configured;
        return local.media == media && remote.media == media;
    }
    void stop() override { }
    QString media;
    int     configured = 0;
};

class AsyncSession : public R::MediaSession {
public:
    std::unique_ptr<R::MediaEndpoint> createEndpoint(const QString &, const QString &media) override
    {
        return std::make_unique<Endpoint>(media);
    }

    int localStarts = 0, answerStarts = 0, applyStarts = 0, cancels = 0;
    R::MediaOperation::Id lastId = 0;
    PrepareCompletion     prepareCompletion;
    ApplyCompletion       applyCompletion;

protected:
    void beginPrepareLocalOffer(R::MediaOperation::Id id, R::MediaEndpoint *, PrepareCompletion completion) override
    {
        check(!prepareCompletion && !applyCompletion, "media operations overlapped");
        ++localStarts;
        lastId            = id;
        prepareCompletion = std::move(completion);
    }
    void beginPrepareAnswer(R::MediaOperation::Id id, R::MediaEndpoint *, const R::Description &remote,
                            PrepareCompletion completion) override
    {
        check(remote.media == QLatin1String("audio"), "remote answer snapshot changed");
        check(!prepareCompletion && !applyCompletion, "media operations overlapped");
        ++answerStarts;
        lastId            = id;
        prepareCompletion = std::move(completion);
    }
    void beginApplyNegotiation(R::MediaOperation::Id id, R::MediaEndpoint *, const R::Description &local,
                               const R::Description &remote, ApplyCompletion completion) override
    {
        check(local.media == QLatin1String("audio") && remote.media == QLatin1String("audio"),
              "apply snapshots changed");
        check(!prepareCompletion && !applyCompletion, "media operations overlapped");
        ++applyStarts;
        lastId          = id;
        applyCompletion = std::move(completion);
    }
    void cancelMediaOperation(R::MediaOperation::Id id) override
    {
        check(id == lastId, "wrong media operation cancelled");
        ++cancels;
        prepareCompletion = {};
        applyCompletion   = {};
    }
};

class LegacySession : public R::MediaSession {
public:
    std::unique_ptr<R::MediaEndpoint> createEndpoint(const QString &, const QString &media) override
    {
        return std::make_unique<Endpoint>(media);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication eventLoop(argc, argv);
    Endpoint         audio(QStringLiteral("audio"));
    AsyncSession     session;

    int localCallbacks = 0, answerCallbacks = 0, applyCallbacks = 0;
    auto local = session.prepareLocalOffer(
        &audio, [&](R::MediaOperation::Id id, std::optional<R::Description> result, R::MediaError error) {
            check(id != 0 && result && !error && result->media == QLatin1String("audio"),
                  "local offer completion corrupted");
            ++localCallbacks;
        });
    auto answer = session.prepareAnswer(
        &audio, description(QStringLiteral("audio")),
        [&](R::MediaOperation::Id, std::optional<R::Description>, R::MediaError) { ++answerCallbacks; });
    auto pendingApply = session.applyNegotiation(
        &audio, description(QStringLiteral("audio")), description(QStringLiteral("audio")),
        [&](R::MediaOperation::Id, R::MediaError) { ++applyCallbacks; });
    check(local && answer && pendingApply && local->id() && answer->id() && pendingApply->id()
              && local->id() != answer->id() && answer->id() != pendingApply->id(),
          "media operation ids are not unique and non-zero");
    check(session.localStarts == 0 && localCallbacks == 0, "media operation started or completed inline");

    pump();
    check(session.localStarts == 1 && session.answerStarts == 0 && session.applyStarts == 0,
          "media session did not serialize initial operations");
    auto firstCompletion = session.prepareCompletion;
    session.prepareCompletion = {};
    firstCompletion(description(QStringLiteral("audio")), {});
    firstCompletion(description(QStringLiteral("audio")), {}); // duplicate backend completion must be ignored
    check(localCallbacks == 0, "media completion callback ran inline");
    pump();
    check(localCallbacks == 1 && session.answerStarts == 1 && session.applyStarts == 0,
          "one-shot completion or serialized start failed");

    // Cancelling queued work never reaches the backend.
    const int cancelsBeforeQueued = session.cancels;
    pendingApply->cancel();
    check(session.cancels == cancelsBeforeQueued, "queued media operation invoked backend cancellation");

    // Cancelling active work revokes completion delivery even if the backend races
    // a stale result after cancellation.
    auto staleAnswer = session.prepareCompletion;
    check(bool(staleAnswer), "active answer completion missing");
    answer->cancel();
    check(session.cancels == cancelsBeforeQueued + 1, "active media operation was not cancelled");
    staleAnswer(description(QStringLiteral("audio")), {});
    pump();
    check(answerCallbacks == 0 && session.applyStarts == 0, "cancelled/stale media callback was delivered");

    auto apply = session.applyNegotiation(
        &audio, description(QStringLiteral("audio")), description(QStringLiteral("audio")),
        [&](R::MediaOperation::Id id, R::MediaError error) {
            check(id == apply->id() && !error, "apply completion corrupted");
            ++applyCallbacks;
        });
    pump();
    check(session.applyStarts == 1 && bool(session.applyCompletion), "apply operation did not start");
    auto applyDone = session.applyCompletion;
    session.applyCompletion = {};
    applyDone({});
    applyDone({});
    check(applyCallbacks == 0, "apply callback ran inline");
    pump();
    check(applyCallbacks == 1, "duplicate apply completion was delivered");

    // A malformed backend completion is an explicit failure, never an empty
    // successful answer.
    int invalidCallbacks = 0;
    auto invalid = session.prepareLocalOffer(
        &audio, [&](R::MediaOperation::Id, std::optional<R::Description> result, R::MediaError error) {
            check(!result && error.code == R::MediaError::Code::Backend,
                  "empty successful preparation was accepted");
            ++invalidCallbacks;
        });
    pump();
    auto invalidDone = session.prepareCompletion;
    session.prepareCompletion = {};
    invalidDone({}, {});
    pump();
    check(invalidCallbacks == 1, "invalid preparation completion was lost");

    // Destroying the handle cancels active work. cancelAll() uses the same backend
    // hook while the derived MediaSession is alive.
    auto disposable = session.prepareLocalOffer(
        &audio, [](R::MediaOperation::Id, std::optional<R::Description>, R::MediaError) { });
    pump();
    const int beforeHandleDrop = session.cancels;
    disposable.reset();
    check(session.cancels == beforeHandleDrop + 1, "operation handle destruction did not cancel backend work");

    auto active = session.prepareLocalOffer(
        &audio, [](R::MediaOperation::Id, std::optional<R::Description>, R::MediaError) { });
    pump();
    const int beforeCancelAll = session.cancels;
    session.cancelAll();
    check(session.cancels == beforeCancelAll + 1, "cancelAll did not cancel active backend work");
    active.reset(); // already cancelled by the session: must not hit backend again
    check(session.cancels == beforeCancelAll + 1, "cancelled session operation was cancelled twice");

    // Compatibility fallback remains asynchronous from the caller's point of view.
    LegacySession legacy;
    Endpoint      legacyAudio(QStringLiteral("audio"));
    int           fallbackPrepared = 0;
    auto fallbackOffer = legacy.prepareLocalOffer(
        &legacyAudio,
        [&](R::MediaOperation::Id, std::optional<R::Description> result, R::MediaError error) {
            check(result && !error, "legacy localOffer fallback failed");
            ++fallbackPrepared;
        });
    check(fallbackPrepared == 0, "legacy preparation completed inline");
    pump();
    check(fallbackPrepared == 1, "legacy preparation fallback did not complete");

    int fallbackApplied = 0;
    auto fallbackApply = legacy.applyNegotiation(
        &legacyAudio, description(QStringLiteral("audio")), description(QStringLiteral("audio")),
        [&](R::MediaOperation::Id, R::MediaError error) {
            check(!error, "legacy configure fallback failed");
            ++fallbackApplied;
        });
    check(fallbackApplied == 0 && legacyAudio.configured == 0, "legacy apply ran inline");
    pump();
    check(fallbackApplied == 1 && legacyAudio.configured == 1, "legacy configure fallback did not complete");

    qInfo("RTP media operation regressions passed");
}
