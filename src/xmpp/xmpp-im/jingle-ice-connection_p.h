// Internal ICE resource ownership. Not a negotiated BUNDLE or public transport API.
#ifndef JINGLE_ICE_CONNECTION_P_H
#define JINGLE_ICE_CONNECTION_P_H

#include "jingle.h"

#include <QObject>
#include <QSharedPointer>
#include <QVector>

namespace XMPP {
class Dtls;
class Ice176;
class UdpPortReserver;
namespace Jingle {
    namespace RTP {
        class SrtpSession;
    }
    namespace SCTP {
        class Association;
    }
    namespace ICE {
        class RawConnection;

        struct Component {
            int                           componentIndex  = 0;
            bool                          initialized     = false;
            bool                          lowOverhead     = false;
            bool                          needDatachannel = false;
            Dtls                         *dtls            = nullptr;
            RTP::SrtpSession             *srtp            = nullptr;
            SCTP::Association            *sctp            = nullptr;
            QSharedPointer<RawConnection> rawConnection;
        };

        // These counters are deliberately independent. ICE restart, DTLS rekey
        // and membership changes invalidate different classes of callbacks.
        struct ConnectionGeneration {
            quint64 iceGeneration      = 0;
            quint64 dtlsEpoch          = 0;
            quint64 membershipRevision = 0;

            bool operator==(const ConnectionGeneration &other) const
            {
                return iceGeneration == other.iceGeneration && dtlsEpoch == other.dtlsEpoch
                    && membershipRevision == other.membershipRevision;
            }
            bool operator!=(const ConnectionGeneration &other) const { return !(*this == other); }
        };

        // No QObject parent: the last strong membership owns destruction. All access,
        // including releasing memberships, must happen on the connection's thread.
        class IceConnection : public QObject {
        public:
            QVector<Component>  components;
            UdpPortReserver    *portReserver = nullptr;
            Ice176             *ice          = nullptr;
            ConnectionGeneration generation;

            ~IceConnection() override;
        };

        // A logical content's strong share in one session-local network association.
        // Move-only so copying a convenience handle cannot accidentally extend the
        // association lifetime as if another content had joined it.
        class ConnectionMembership {
        public:
            ConnectionMembership() = default;
            ConnectionMembership(QSharedPointer<IceConnection> connection, quint64 associationId, ContentKey content) :
                connection_(std::move(connection)), associationId_(associationId), content_(std::move(content))
            {
            }
            ConnectionMembership(const ConnectionMembership &)            = delete;
            ConnectionMembership &operator=(const ConnectionMembership &) = delete;
            ConnectionMembership(ConnectionMembership &&)                 = default;
            ConnectionMembership &operator=(ConnectionMembership &&)      = default;

            explicit operator bool() const { return !connection_.isNull(); }
            IceConnection *connection() const { return connection_.data(); }
            quint64        associationId() const { return associationId_; }
            const ContentKey &content() const { return content_; }
            ConnectionGeneration generation() const
            {
                return connection_ ? connection_->generation : ConnectionGeneration {};
            }
            void reset()
            {
                connection_.reset();
                associationId_ = 0;
                content_       = {};
            }

        private:
            QSharedPointer<IceConnection> connection_;
            quint64                       associationId_ = 0;
            ContentKey                    content_;
        };
    }
}
}
#endif
