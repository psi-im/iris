/*
 * jignle-session.h - Jingle Session
 * Copyright (C) 2019  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef JINGLE_SESSION_H
#define JINGLE_SESSION_H

#include <iris/iris_export.h>

#include <iris/xmpp-im/jingle-application.h>
#include <iris/xmpp-im/jingle-transport.h>
#include <iris/xmpp-im/xmpp_features.h>

#include <algorithm>
#include <functional>
#include <memory>

namespace XMPP { namespace Jingle {

    // Callback-only collision coordinator. Context, decision and event types are
    // supplied by its owner; the coordinator deliberately knows nothing about
    // Jingle actions, XMPP stanzas, roles or any application/transport policy.
    template <typename Context, typename Decision, typename Event> class TieBreakResolver {
    public:
        enum class ResolutionState { Waiting, Finished };

        struct Plan {
            Decision                                      decision {};
            std::function<void()>                         immediate;
            std::function<ResolutionState(const Event &)> advance;
        };

        struct Callbacks {
            std::function<bool(const Context &)> conflicts;
            std::function<Plan(const Context &)> resolve;
        };

        struct Result {
            bool     handled    = false;
            Decision decision   {};
            quint64  resolution = 0;
        };

        using Registration = quint64;

        Registration registerResolver(Callbacks callbacks)
        {
            if (!callbacks.conflicts || !callbacks.resolve)
                return 0;
            const auto id = ++nextRegistration_;
            resolvers_.append(RegisteredResolver { id, std::make_shared<Callbacks>(std::move(callbacks)) });
            return id;
        }

        void unregisterResolver(Registration registration)
        {
            if (!registration)
                return;
            resolvers_.erase(std::remove_if(resolvers_.begin(), resolvers_.end(), [registration](const auto &entry) {
                                 return entry.registration == registration;
                             }),
                             resolvers_.end());
        }

        Result resolve(const Context &context)
        {
            // Copy only shared callback owners so callbacks may unregister
            // themselves while mutable callback state survives subsequent calls.
            const auto entries = resolvers_;
            for (const auto &entry : entries) {
                const auto callbacks = entry.callbacks;
                if (!callbacks || !callbacks->conflicts(context))
                    continue;

                auto plan = callbacks->resolve(context);
                if (plan.immediate)
                    plan.immediate();

                quint64 resolution = 0;
                if (plan.advance) {
                    resolution = ++nextResolution_;
                    resolutions_.insert(resolution, ActiveResolution { std::move(plan.advance) });
                }
                return Result { true, plan.decision, resolution };
            }
            return {};
        }

        void notify(quint64 resolution, const Event &event)
        {
            if (!resolution)
                return;
            auto it = resolutions_.find(resolution);
            if (it == resolutions_.end())
                return;

            // Remove the machine while invoking external code. Reentrant notify()
            // of the same id is therefore harmless, and moving rather than copying
            // std::function preserves mutable captures across events.
            auto active = std::move(it.value());
            resolutions_.erase(it);
            if (active.advance && active.advance(event) == ResolutionState::Waiting)
                resolutions_.insert(resolution, std::move(active));
        }

        void notifyAll(const Event &event)
        {
            const auto ids = resolutions_.keys();
            for (auto id : ids)
                notify(id, event);
        }

        bool hasResolution(quint64 resolution) const { return resolutions_.contains(resolution); }
        int  activeResolutionCount() const { return resolutions_.size(); }

    private:
        struct RegisteredResolver {
            Registration               registration;
            std::shared_ptr<Callbacks> callbacks;
        };
        struct ActiveResolution {
            std::function<ResolutionState(const Event &)> advance;
        };

        QList<RegisteredResolver>            resolvers_;
        QHash<quint64, ActiveResolution>     resolutions_;
        quint64                              nextRegistration_ = 0;
        quint64                              nextResolution_   = 0;
    };

    // class Manager;
    class Application;

    // Ordered XEP-0338 group. Multiple groups may have the same semantics.
    struct ContentGroup {
        QString     semantics;
        QStringList contents;
    };

    class IRIS_EXPORT Session : public QObject {
        Q_OBJECT
    public:
        // Incoming sessions are not registered in Jingle Manager until their initial contents are validated. A valid
        // incoming session remains in Created state while it waits for local accept() or terminate().

        Session(Manager *manager, const Jid &peer, Origin role = Origin::Initiator);
        ~Session();

        Manager *manager() const;
        State    state() const;

        Jid     me() const;
        Jid     peer() const;
        Jid     initiator() const;
        Jid     responder() const;
        QString sid() const;

        Origin   role() const; // my role in session: initiator or responder
        Origin   peerRole() const;
        bool     checkPeerCaps(const QString &ns) const;
        Features peerFeatures() const;

        bool isGroupingAllowed() const;

        std::optional<Stanza::Error> lastError() const;

        /**
         * @brief Create a local application for a new Jingle content.
         * @param ns application description namespace
         * @param senders value represented by the Jingle content `senders` attribute
         * @return a new application, or nullptr if the namespace is not registered
         *
         * The returned application is not added to the session yet. Configure it first, then pass it to
         * addContent().
         */
        Application *newContent(const QString &ns, Origin senders = Origin::Both);
        // get registered content if any
        Application *content(const QString &contentName, Origin creator);

        /**
         * @brief Add a previously created local application to the session.
         *
         * The session takes responsibility for the application lifetime. If negotiation has already started, the
         * application is prepared immediately so it can be sent as content-add.
         */
        void                                   addContent(Application *content);
        const QMap<ContentKey, Application *> &contentList() const;
        void                                   setGrouping(const QString &groupType, const QStringList &group);
        // Local signaling proposal, not proof of an established shared transport.
        bool                setGroupings(const QList<ContentGroup> &groups);
        QList<ContentGroup> groupings() const;
        // Last successfully parsed initial peer offer/answer. Never auto-accepted.
        QList<ContentGroup> remoteGroupings() const;

        ApplicationManagerPad::Ptr applicationPad(const QString &ns);
        TransportManagerPad::Ptr   transportPad(const QString &ns);

        QSharedPointer<Transport> newOutgoingTransport(const QString &ns);

        QString     preferredApplication() const;
        QStringList allApplicationTypes() const;

        void setLocalJid(const Jid &jid); // w/o real use case the implementation is rather stub

        /// Accept a validated incoming session and start preparing its initial contents for session-accept.
        void accept();

        /// Start preparing an outgoing session. session-initiate is sent when all initial contents are ready.
        void initiate();
        void terminate(Reason::Condition cond, const QString &comment = QString());

        // allocates or returns existing pads
        ApplicationManagerPad::Ptr applicationPadFactory(const QString &ns);
        TransportManagerPad::Ptr   transportPadFactory(const QString &ns);
    signals:
        void managerPadAdded(const QString &ns);
        void initiated();

        /**
         * Emitted when the initial Jingle negotiation has been accepted and applications are allowed to start their
         * transports. It does not mean that every application Connection is already active.
         */
        void activated();
        void terminated();
        void newContentReceived();

    private:
        friend class Application;
        friend class Manager;
        friend class PublicationManager;
        friend class JTPush;

        enum class TieBreakIncoming { Pass, Reject };
        enum class TieBreakEvent { IncomingApplied, IncomingRejected, LocalCompleted, Wake };
        struct TieBreakContext {
            Action incomingAction = Action::NoAction;
        };
        using SessionTieBreakResolver = TieBreakResolver<TieBreakContext, TieBreakIncoming, TieBreakEvent>;

        void ensureTieBreakResolvers() const
        {
            if (tieBreakResolversReady_)
                return;
            tieBreakResolversReady_ = true;

            // First consumer of the generic framework. transport-replace keeps
            // its existing handler for now because its tie-break path also
            // performs transport-selection side effects that need richer owner
            // context before it can be migrated without changing behaviour.
            tieBreakResolver_.registerResolver(SessionTieBreakResolver::Callbacks {
                [this](const TieBreakContext &context) {
                    return context.incomingAction == Action::ContentModify && *contentModifyInFlight_ > 0;
                },
                [this](const TieBreakContext &) {
                    SessionTieBreakResolver::Plan plan;
                    plan.decision = role() == Origin::Initiator ? TieBreakIncoming::Reject : TieBreakIncoming::Pass;
                    return plan;
                } });
        }

        // Application callbacks keep this token alive until the corresponding
        // content-modify IQ has completed, even if the Application is removed first.
        std::shared_ptr<void> trackContentModify()
        {
            ensureTieBreakResolvers();
            auto counter = contentModifyInFlight_;
            ++*counter;
            return std::shared_ptr<void>(counter.get(), [counter](int *) { --*counter; });
        }

        bool shouldTieBreakIncoming(Action action) const
        {
            ensureTieBreakResolvers();
            const auto result = tieBreakResolver_.resolve(TieBreakContext { action });
            if (!result.handled || result.decision != TieBreakIncoming::Reject)
                return false;
            if (result.resolution)
                tieBreakResolver_.notify(result.resolution, TieBreakEvent::IncomingRejected);
            return true;
        }

        QString                                   reserveSid();
        bool                                      incomingInitiate(const Jingle &jingle, const QDomElement &jingleEl);
        bool                                      updateFromXml(Action action, const QDomElement &jingleEl);
        static std::optional<QList<ContentGroup>> parseGroupings(const QDomElement &jingleEl);
        static bool validBundleAnswer(const QList<ContentGroup> &offer, const QList<ContentGroup> &answer);
        bool        validLocalGroupings() const;

        std::shared_ptr<int>           contentModifyInFlight_ = std::make_shared<int>(0);
        mutable SessionTieBreakResolver tieBreakResolver_;
        mutable bool                    tieBreakResolversReady_ = false;

        class Private;
        std::unique_ptr<Private> d;
    };
}}

#endif // JINGLE_SESSION_H
