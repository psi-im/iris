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

    // Generic callback-driven collision coordinator. It intentionally knows
    // nothing about Jingle/XEP tie-break semantics: registered owners decide
    // whether a collision exists, what the immediate disposition is and, when
    // needed, how a per-collision resolution state machine advances.
    class TieBreakResolver {
    public:
        enum class IncomingDisposition { Pass, Reject };
        enum class Event { IncomingApplied, IncomingRejected, LocalCompleted, Wake };
        enum class ResolutionState { Waiting, Finished };

        struct Context {
            Action             incomingAction = Action::NoAction;
            Action             localAction    = Action::NoAction;
            const QDomElement *incoming       = nullptr;
            const QDomElement *local          = nullptr;
        };

        struct Plan {
            IncomingDisposition                          incoming = IncomingDisposition::Pass;
            std::function<void()>                        immediate;
            std::function<ResolutionState(Event event)> advance;
        };

        struct Callbacks {
            std::function<bool(const Context &)> conflicts;
            std::function<Plan(const Context &)> resolve;
        };

        struct Decision {
            IncomingDisposition incoming   = IncomingDisposition::Pass;
            quint64             resolution = 0;
        };

        using Registration = quint64;

        Registration registerResolver(Action action, Callbacks callbacks)
        {
            if (!callbacks.conflicts || !callbacks.resolve)
                return 0;
            const auto id = ++nextRegistration_;
            resolvers_[action].append(RegisteredResolver { id, std::move(callbacks) });
            return id;
        }

        void unregisterResolver(Registration registration)
        {
            if (!registration)
                return;
            for (auto it = resolvers_.begin(); it != resolvers_.end();) {
                auto &entries = it.value();
                entries.erase(std::remove_if(entries.begin(), entries.end(), [registration](const auto &entry) {
                                  return entry.registration == registration;
                              }),
                              entries.end());
                if (entries.isEmpty())
                    it = resolvers_.erase(it);
                else
                    ++it;
            }
        }

        Decision resolve(const Context &context)
        {
            // Copy registrations so callbacks may safely register/unregister
            // other resolvers while this collision is being decided.
            const auto entries = resolvers_.value(context.incomingAction);
            for (const auto &entry : entries) {
                if (!entry.callbacks.conflicts(context))
                    continue;

                auto plan = entry.callbacks.resolve(context);
                if (plan.immediate)
                    plan.immediate();

                quint64 resolution = 0;
                if (plan.advance) {
                    resolution = ++nextResolution_;
                    resolutions_.insert(resolution,
                                        ActiveResolution { context.incomingAction, std::move(plan.advance) });
                }
                return Decision { plan.incoming, resolution };
            }
            return {};
        }

        void notify(quint64 resolution, Event event)
        {
            if (!resolution)
                return;
            auto it = resolutions_.find(resolution);
            if (it == resolutions_.end())
                return;

            auto advance = it->advance;
            if (!advance || advance(event) == ResolutionState::Finished)
                resolutions_.remove(resolution);
        }

        void notify(Action action, Event event)
        {
            QList<quint64> ids;
            for (auto it = resolutions_.cbegin(); it != resolutions_.cend(); ++it) {
                if (it->action == action)
                    ids.append(it.key());
            }
            for (auto id : ids)
                notify(id, event);
        }

        bool hasResolution(quint64 resolution) const { return resolutions_.contains(resolution); }
        int  activeResolutionCount() const { return resolutions_.size(); }

    private:
        struct RegisteredResolver {
            Registration registration;
            Callbacks    callbacks;
        };
        struct ActiveResolution {
            Action                                      action;
            std::function<ResolutionState(Event event)> advance;
        };

        QHash<Action, QList<RegisteredResolver>> resolvers_;
        QHash<quint64, ActiveResolution>         resolutions_;
        quint64                                  nextRegistration_ = 0;
        quint64                                  nextResolution_   = 0;
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

        void ensureTieBreakResolvers() const
        {
            if (tieBreakResolversReady_)
                return;
            tieBreakResolversReady_ = true;

            auto registerInitiatorWins = [this](Action action, std::function<bool()> conflicts) {
                tieBreakResolver_.registerResolver(
                    action,
                    TieBreakResolver::Callbacks {
                        [conflicts = std::move(conflicts)](const TieBreakResolver::Context &) { return conflicts(); },
                        [this](const TieBreakResolver::Context &) {
                            TieBreakResolver::Plan plan;
                            plan.incoming = role() == Origin::Initiator ? TieBreakResolver::IncomingDisposition::Reject
                                                                       : TieBreakResolver::IncomingDisposition::Pass;
                            return plan;
                        } });
            };

            registerInitiatorWins(Action::ContentModify,
                                  [this]() { return *contentModifyInFlight_ > 0; });

            registerInitiatorWins(Action::TransportReplace, [this]() {
                for (auto app : contentList()) {
                    if (!app)
                        continue;
                    auto transport = app->transport();
                    if (transport && transport->isLocal() && transport->state() == State::Unacked)
                        return true;
                }
                return false;
            });
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
            TieBreakResolver::Context context;
            context.incomingAction = action;
            context.localAction    = action;
            const auto decision    = tieBreakResolver_.resolve(context);
            if (decision.incoming == TieBreakResolver::IncomingDisposition::Reject) {
                tieBreakResolver_.notify(decision.resolution, TieBreakResolver::Event::IncomingRejected);
                return true;
            }
            return false;
        }

        QString                                   reserveSid();
        bool                                      incomingInitiate(const Jingle &jingle, const QDomElement &jingleEl);
        bool                                      updateFromXml(Action action, const QDomElement &jingleEl);
        static std::optional<QList<ContentGroup>> parseGroupings(const QDomElement &jingleEl);
        static bool validBundleAnswer(const QList<ContentGroup> &offer, const QList<ContentGroup> &answer);
        bool        validLocalGroupings() const;

        std::shared_ptr<int>      contentModifyInFlight_ = std::make_shared<int>(0);
        mutable TieBreakResolver  tieBreakResolver_;
        mutable bool              tieBreakResolversReady_ = false;

        class Private;
        std::unique_ptr<Private> d;
    };
}}

#endif // JINGLE_SESSION_H
