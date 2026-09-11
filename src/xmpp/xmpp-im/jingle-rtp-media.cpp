// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp.h"

#include <QThread>
#include <QTimer>

namespace XMPP::Jingle::RTP {
namespace {
    MediaError backendError(const QString &text)
    {
        return { MediaError::Code::Backend, text };
    }

    MediaError unsupportedError(const QString &text)
    {
        return { MediaError::Code::Unsupported, text };
    }
}

class MediaOperation::Private {
public:
    Private(Id id, MediaSession *session) : id(id), session(session) { }
    Id                     id;
    QPointer<MediaSession> session;
    bool                   cancelled = false;
};

MediaOperation::MediaOperation(Id id, MediaSession *session) : d(std::make_unique<Private>(id, session)) { }
MediaOperation::~MediaOperation() { cancel(); }
MediaOperation::Id MediaOperation::id() const { return d ? d->id : 0; }
void MediaOperation::cancel()
{
    if (!d || d->cancelled)
        return;
    d->cancelled = true;
    if (d->session)
        d->session->cancelOperation(d->id);
    d->session.clear();
}

class MediaSession::Private {
public:
    enum class Kind { PrepareLocalOffer, PrepareAnswer, ApplyNegotiation };
    struct State {
        MediaOperation::Id          id = 0;
        Kind                        kind;
        MediaEndpoint              *endpoint = nullptr;
        std::optional<Description>  local;
        std::optional<Description>  remote;
        PrepareCallback             prepareCallback;
        ApplyCallback               applyCallback;
    };

    MediaOperation::Id allocateId()
    {
        ++nextId;
        if (!nextId)
            ++nextId;
        return nextId;
    }

    QList<std::shared_ptr<State>> pending;
    std::shared_ptr<State>        active;
    MediaOperation::Id            nextId         = 0;
    bool                          startScheduled = false;
};

MediaSession::MediaSession(QObject *parent) : QObject(parent), d(std::make_unique<Private>()) { }
MediaSession::~MediaSession()
{
    // Pad calls cancelAll() before destruction while virtual dispatch to the
    // adapter is still valid. This final cleanup only suppresses queued delivery.
    d->pending.clear();
    d->active.reset();
}

std::unique_ptr<MediaOperation> MediaSession::prepareLocalOffer(MediaEndpoint *endpoint, PrepareCallback callback)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!endpoint || !callback)
        return {};
    auto state             = std::make_shared<Private::State>();
    state->id              = d->allocateId();
    state->kind            = Private::Kind::PrepareLocalOffer;
    state->endpoint        = endpoint;
    state->prepareCallback = std::move(callback);
    d->pending.append(state);
    auto operation = std::unique_ptr<MediaOperation>(new MediaOperation(state->id, this));
    scheduleNext();
    return operation;
}

std::unique_ptr<MediaOperation> MediaSession::prepareAnswer(MediaEndpoint *endpoint, const Description &remoteSnapshot,
                                                            PrepareCallback callback)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!endpoint || !callback)
        return {};
    auto state             = std::make_shared<Private::State>();
    state->id              = d->allocateId();
    state->kind            = Private::Kind::PrepareAnswer;
    state->endpoint        = endpoint;
    state->remote          = remoteSnapshot;
    state->prepareCallback = std::move(callback);
    d->pending.append(state);
    auto operation = std::unique_ptr<MediaOperation>(new MediaOperation(state->id, this));
    scheduleNext();
    return operation;
}

std::unique_ptr<MediaOperation> MediaSession::applyNegotiation(MediaEndpoint *endpoint, const Description &local,
                                                               const Description &remote, ApplyCallback callback)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!endpoint || !callback)
        return {};
    auto state          = std::make_shared<Private::State>();
    state->id           = d->allocateId();
    state->kind         = Private::Kind::ApplyNegotiation;
    state->endpoint     = endpoint;
    state->local        = local;
    state->remote       = remote;
    state->applyCallback = std::move(callback);
    d->pending.append(state);
    auto operation = std::unique_ptr<MediaOperation>(new MediaOperation(state->id, this));
    scheduleNext();
    return operation;
}

void MediaSession::cancelOperation(MediaOperation::Id id)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->active && d->active->id == id) {
        d->active.reset();
        QPointer<MediaSession> guard(this);
        cancelMediaOperation(id);
        if (guard)
            scheduleNext();
        return;
    }
    for (auto it = d->pending.begin(); it != d->pending.end(); ++it) {
        if ((*it)->id == id) {
            d->pending.erase(it);
            scheduleNext();
            return;
        }
    }
}

void MediaSession::cancelAll()
{
    Q_ASSERT(QThread::currentThread() == thread());
    d->pending.clear();
    if (!d->active)
        return;
    const auto id = d->active->id;
    d->active.reset();
    cancelMediaOperation(id);
}

void MediaSession::scheduleNext()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->startScheduled || d->active || d->pending.isEmpty())
        return;
    d->startScheduled = true;
    QPointer<MediaSession> guard(this);
    QTimer::singleShot(0, this, [guard] {
        if (!guard)
            return;
        guard->d->startScheduled = false;
        guard->startNext();
    });
}

void MediaSession::startNext()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->active || d->pending.isEmpty())
        return;
    auto state = d->pending.takeFirst();
    d->active  = state;

    QPointer<MediaSession> guard(this);
    const auto             id = state->id;
    switch (state->kind) {
    case Private::Kind::PrepareLocalOffer:
        beginPrepareLocalOffer(id, state->endpoint,
                               [guard, id](std::optional<Description> result, MediaError error) mutable {
                                   if (!guard)
                                       return;
                                   Q_ASSERT(QThread::currentThread() == guard->thread());
                                   QTimer::singleShot(0, guard, [guard, id, result = std::move(result),
                                                                 error = std::move(error)]() mutable {
                                       if (guard)
                                           guard->finishPrepared(id, std::move(result), std::move(error));
                                   });
                               });
        break;
    case Private::Kind::PrepareAnswer:
        beginPrepareAnswer(id, state->endpoint, *state->remote,
                           [guard, id](std::optional<Description> result, MediaError error) mutable {
                               if (!guard)
                                   return;
                               Q_ASSERT(QThread::currentThread() == guard->thread());
                               QTimer::singleShot(0, guard, [guard, id, result = std::move(result),
                                                             error = std::move(error)]() mutable {
                                   if (guard)
                                       guard->finishPrepared(id, std::move(result), std::move(error));
                               });
                           });
        break;
    case Private::Kind::ApplyNegotiation:
        beginApplyNegotiation(id, state->endpoint, *state->local, *state->remote,
                              [guard, id](MediaError error) mutable {
                                  if (!guard)
                                      return;
                                  Q_ASSERT(QThread::currentThread() == guard->thread());
                                  QTimer::singleShot(0, guard, [guard, id, error = std::move(error)]() mutable {
                                      if (guard)
                                          guard->finishApplied(id, std::move(error));
                                  });
                              });
        break;
    }
}

void MediaSession::finishPrepared(MediaOperation::Id id, std::optional<Description> result, MediaError error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->active || d->active->id != id
        || (d->active->kind != Private::Kind::PrepareLocalOffer && d->active->kind != Private::Kind::PrepareAnswer))
        return; // cancelled, stale, duplicate, or wrong-kind completion

    const bool hasError = bool(error);
    if (result.has_value() == hasError) {
        result.reset();
        error = backendError(QStringLiteral("Media backend returned an invalid preparation result"));
    }

    auto callback = std::move(d->active->prepareCallback);
    d->active.reset();
    QPointer<MediaSession> guard(this);
    if (callback)
        callback(id, std::move(result), std::move(error));
    if (guard)
        scheduleNext();
}

void MediaSession::finishApplied(MediaOperation::Id id, MediaError error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->active || d->active->id != id || d->active->kind != Private::Kind::ApplyNegotiation)
        return; // cancelled, stale, duplicate, or wrong-kind completion
    auto callback = std::move(d->active->applyCallback);
    d->active.reset();
    QPointer<MediaSession> guard(this);
    if (callback)
        callback(id, std::move(error));
    if (guard)
        scheduleNext();
}

void MediaSession::beginPrepareLocalOffer(MediaOperation::Id, MediaEndpoint *endpoint, PrepareCompletion completion)
{
    if (!endpoint) {
        completion({}, backendError(QStringLiteral("Media endpoint disappeared")));
        return;
    }
    completion(endpoint->localOffer(), {});
}

void MediaSession::beginPrepareAnswer(MediaOperation::Id, MediaEndpoint *endpoint, const Description &remoteSnapshot,
                                      PrepareCompletion completion)
{
    if (!endpoint) {
        completion({}, backendError(QStringLiteral("Media endpoint disappeared")));
        return;
    }
    auto answer = endpoint->makeAnswer(remoteSnapshot);
    if (!answer) {
        completion({}, unsupportedError(QStringLiteral("Media backend rejected the remote offer")));
        return;
    }
    completion(std::move(answer), {});
}

void MediaSession::beginApplyNegotiation(MediaOperation::Id, MediaEndpoint *endpoint, const Description &local,
                                         const Description &remote, ApplyCompletion completion)
{
    if (!endpoint) {
        completion(backendError(QStringLiteral("Media endpoint disappeared")));
        return;
    }
    completion(endpoint->configure(local, remote)
                   ? MediaError {}
                   : backendError(QStringLiteral("Media backend failed to apply negotiated parameters")));
}

}
