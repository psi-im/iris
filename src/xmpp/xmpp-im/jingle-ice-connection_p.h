// Internal ICE resource ownership. Not a negotiated BUNDLE or public transport API.
#ifndef JINGLE_ICE_CONNECTION_P_H
#define JINGLE_ICE_CONNECTION_P_H

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

        // No QObject parent: the last strong membership owns destruction. All access,
        // including releasing memberships, must happen on the connection's thread.
        class IceConnection : public QObject {
        public:
            QVector<Component> components;
            UdpPortReserver   *portReserver = nullptr;
            Ice176            *ice          = nullptr;

            ~IceConnection() override;
        };
    }
}
}
#endif
