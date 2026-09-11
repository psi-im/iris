// Internal bridge from a validated GroupPlan to session-local ICE memberships.
#ifndef JINGLE_ICE_GROUP_P_H
#define JINGLE_ICE_GROUP_P_H

#include "jingle-group-negotiation_p.h"
#include "jingle-ice-connection_p.h"

#include <optional>
#include <vector>

namespace XMPP { namespace Jingle { namespace ICE {

    class ConnectionGroupTransaction {
    public:
        struct Entry {
            int                  planAssociationId = -1;
            ContentKey           content;
            ConnectionMembership membership;
        };

        ConnectionGroupTransaction() = default;
        ConnectionGroupTransaction(const ConnectionGroupTransaction &)            = delete;
        ConnectionGroupTransaction &operator=(const ConnectionGroupTransaction &) = delete;
        ConnectionGroupTransaction(ConnectionGroupTransaction &&)                 = default;
        ConnectionGroupTransaction &operator=(ConnectionGroupTransaction &&)      = default;

        static std::optional<ConnectionGroupTransaction> commit(const GroupPlan &plan, ConnectionRegistry &registry)
        {
            if (!plan.readyToCommit())
                return std::nullopt;

            ConnectionGroupTransaction result;
            for (const auto &association : plan.associations()) {
                if (association.members.isEmpty() || association.owner != association.members.first())
                    return std::nullopt;

                auto ownerMembership = registry.create(association.owner);
                if (!ownerMembership)
                    return std::nullopt;
                const auto liveAssociationId = ownerMembership.associationId();
                result.entries_.push_back(
                    Entry { association.id, association.owner, std::move(ownerMembership) });

                for (qsizetype index = 1; index < association.members.size(); ++index) {
                    const auto &content = association.members.at(index);
                    auto membership = registry.attach(liveAssociationId, content);
                    if (!membership)
                        return std::nullopt;
                    result.entries_.push_back(Entry { association.id, content, std::move(membership) });
                }
            }
            return result;
        }

        qsizetype size() const { return qsizetype(entries_.size()); }

        IceConnection *connectionFor(const ContentKey &content) const
        {
            for (const auto &entry : entries_) {
                if (entry.content == content)
                    return entry.membership.connection();
            }
            return nullptr;
        }

        quint64 associationIdFor(const ContentKey &content) const
        {
            for (const auto &entry : entries_) {
                if (entry.content == content)
                    return entry.membership.associationId();
            }
            return 0;
        }

        bool release(const ContentKey &content)
        {
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->content == content) {
                    entries_.erase(it);
                    return true;
                }
            }
            return false;
        }

    private:
        std::vector<Entry> entries_;
    };

}}}

#endif // JINGLE_ICE_GROUP_P_H
