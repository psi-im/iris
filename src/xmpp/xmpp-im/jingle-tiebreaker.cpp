/*
 * jingle-tiebreaker.cpp - Session-scoped Jingle tie-break coordination
 * Copyright (C) 2026  Sergei Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "jingle-tiebreaker.h"

#include <QDomElement>
#include <QHash>

namespace XMPP { namespace Jingle {

    struct TieBreaker::SharedState {
        struct ResolverEntry {
  Action    action = Action::NoAction;
  Resolver *resolver = nullptr;
  int       postponed = 0;
        };
        struct Transaction {
  quint64                      id = 0;
  Action                       action = Action::NoAction;
  QDomElement                  localData;
  bool                         finished = false;
  bool                         callbacksFinished = false;
  std::optional<Stanza::Error> error;
        };
        struct PendingResolution {
  quint64                     id = 0;
  quint64                     transaction = 0;
  QList<quint64>              resolvers;
  std::optional<RemoteResult> remoteResult;
        };

        QHash<quint64, ResolverEntry>      resolvers;
        QHash<quint64, Transaction>        transactions;
        QHash<quint64, PendingResolution> resolutions;
        quint64                            currentOutgoing = 0;
        quint64                            nextResolver = 0;
        quint64                            nextTransaction = 0;
        quint64                            nextResolution = 0;

        QList<quint64> resolutionsFor(quint64 transaction) const
        {
  QList<quint64> result;
  for (auto it = resolutions.cbegin(); it != resolutions.cend(); ++it) {
      if (it->transaction == transaction)
          result.append(it.key());
  }
  return result;
        }

        void releaseResolution(quint64 id)
        {
  auto it = resolutions.find(id);
  if (it == resolutions.end())
      return;
  const auto resolverIds = it->resolvers;
  resolutions.erase(it);
  for (auto resolverId : resolverIds) {
      auto resolver = resolvers.find(resolverId);
      if (resolver != resolvers.end() && resolver->postponed > 0)
          --resolver->postponed;
  }
        }

        void maybeReleaseTransaction(quint64 id)
        {
  auto it = transactions.find(id);
  if (it == transactions.end() || !it->finished || !it->callbacksFinished)
      return;
  if (!resolutionsFor(id).isEmpty())
      return;
  transactions.erase(it);
        }

        void tryRetry(quint64 resolutionId)
        {
  auto resolution = resolutions.find(resolutionId);
  if (resolution == resolutions.end() || !resolution->remoteResult)
      return;
  auto transaction = transactions.find(resolution->transaction);
  if (transaction == transactions.end() || !transaction->finished || !transaction->callbacksFinished
      || !transaction->error)
      return;

  // Copy everything a callback may need before removing the pending
  // resolution. retry() may synchronously unregister resolvers or
  // create another outgoing Jingle action.
  const auto transactionId = transaction->id;
  const auto localData     = transaction->localData;
  const auto localError    = *transaction->error;
  const auto remoteResult  = *resolution->remoteResult;
  const auto resolverIds   = resolution->resolvers;
  releaseResolution(resolutionId);
  maybeReleaseTransaction(transactionId);

  const RetryContext context { localData, localError, remoteResult };
  for (auto resolverId : resolverIds) {
      auto entry = resolvers.find(resolverId);
      if (entry != resolvers.end() && entry->resolver)
          entry->resolver->retry(context);
  }
        }
    };

    TieBreaker::Registration::Registration(std::weak_ptr<SharedState> state, quint64 id) :
        state_(std::move(state)), id_(id)
    {
    }

    TieBreaker::Registration::~Registration() { reset(); }

    TieBreaker::Registration::Registration(Registration &&other) noexcept :
        state_(std::move(other.state_)), id_(other.id_)
    {
        other.id_ = 0;
    }

    TieBreaker::Registration &TieBreaker::Registration::operator=(Registration &&other) noexcept
    {
        if (this == &other)
  return *this;
        reset();
        state_    = std::move(other.state_);
        id_       = other.id_;
        other.id_ = 0;
        return *this;
    }

    void TieBreaker::Registration::reset()
    {
        if (!id_)
  return;
        if (auto state = state_.lock())
  state->resolvers.remove(id_);
        id_ = 0;
        state_.reset();
    }

    bool TieBreaker::Registration::isPostponed() const
    {
        if (!id_)
  return false;
        if (auto state = state_.lock()) {
  const auto it = state->resolvers.constFind(id_);
  return it != state->resolvers.cend() && it->postponed > 0;
        }
        return false;
    }

    TieBreaker::TieBreaker() : state_(std::make_shared<SharedState>()) { }
    TieBreaker::~TieBreaker() = default;

    TieBreaker::Registration TieBreaker::registerResolver(Action action, Resolver *resolver)
    {
        if (!resolver || action == Action::NoAction)
  return {};
        auto id = ++state_->nextResolver;
        state_->resolvers.insert(id, SharedState::ResolverEntry { action, resolver, 0 });
        return Registration(state_, id);
    }

    quint64 TieBreaker::outgoingStarted(Action action, const QDomElement &localData)
    {
        const auto id = ++state_->nextTransaction;
        state_->transactions.insert(id, SharedState::Transaction { id, action, localData });
        state_->currentOutgoing = id;
        return id;
    }

    void TieBreaker::outgoingFinished(quint64 transactionId, const std::optional<Stanza::Error> &error)
    {
        auto transaction = state_->transactions.find(transactionId);
        if (transaction == state_->transactions.end())
  return;
        if (state_->currentOutgoing == transactionId)
  state_->currentOutgoing = 0; // clear collision lifetime before owner callbacks
        transaction->finished = true;
        transaction->error    = error;

        if (!error) {
  // The peer accepted our proposal. A postponed local intent therefore
  // needs no tie-break recovery, irrespective of the remote IQ outcome.
  const auto resolutions = state_->resolutionsFor(transactionId);
  for (auto resolution : resolutions)
      state_->releaseResolution(resolution);
        } else {
  const auto resolutions = state_->resolutionsFor(transactionId);
  for (auto resolution : resolutions)
      state_->tryRetry(resolution);
        }
        state_->maybeReleaseTransaction(transactionId);
    }

    void TieBreaker::outgoingCallbacksFinished(quint64 transactionId)
    {
        auto transaction = state_->transactions.find(transactionId);
        if (transaction == state_->transactions.end())
  return;
        transaction->callbacksFinished = true;
        const auto resolutions          = state_->resolutionsFor(transactionId);
        for (auto resolution : resolutions)
  state_->tryRetry(resolution);
        state_->maybeReleaseTransaction(transactionId);
    }

    TieBreaker::Resolution TieBreaker::resolveIncoming(Action action, const QDomElement &remoteData)
    {
        auto transaction = state_->transactions.constFind(state_->currentOutgoing);
        if (transaction == state_->transactions.cend() || transaction->action != action)
  return {};

        QList<quint64> resolverIds;
        for (auto it = state_->resolvers.cbegin(); it != state_->resolvers.cend(); ++it) {
  if (it->action == action)
      resolverIds.append(it.key());
        }

        QList<quint64> postponed;
        for (auto resolverId : resolverIds) {
  auto entry = state_->resolvers.find(resolverId);
  if (entry == state_->resolvers.end() || !entry->resolver)
      continue;
  const auto solution = entry->resolver->resolve(transaction->localData, remoteData);
  if (solution == Solution::Break)
      return { Solution::Break, 0 };
  if (solution == Solution::Postpone && state_->resolvers.contains(resolverId))
      postponed.append(resolverId);
        }
        if (postponed.isEmpty())
  return {};

        const auto id = ++state_->nextResolution;
        state_->resolutions.insert(id, SharedState::PendingResolution { id, transaction->id, postponed, {} });
        for (auto resolverId : postponed) {
  auto entry = state_->resolvers.find(resolverId);
  if (entry != state_->resolvers.end())
      ++entry->postponed;
        }
        return { Solution::Postpone, id };
    }

    void TieBreaker::incomingFinished(quint64 resolutionId, RemoteResult result)
    {
        if (!resolutionId)
  return;
        auto resolution = state_->resolutions.find(resolutionId);
        if (resolution == state_->resolutions.end())
  return;
        resolution->remoteResult = result;
        state_->tryRetry(resolutionId);
    }

    void TieBreaker::clear()
    {
        state_->currentOutgoing = 0;
        state_->transactions.clear();
        state_->resolutions.clear();
        for (auto it = state_->resolvers.begin(); it != state_->resolvers.end(); ++it)
  it->postponed = 0;
    }

}}
