// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_RTP_ROUTER_P_H
#define JINGLE_RTP_ROUTER_P_H

#include "jingle-rtp-srtp.h"
#include "jingle.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <optional>

namespace XMPP::Jingle::RTP {

// Authenticated BUNDLE packet demultiplexing. The caller may pass packets here
// only after SRTP/SRTCP authentication. This class never decrypts packets and
// never broadcasts an ambiguous packet to multiple contents.
class BundleRouter {
public:
    static constexpr int MaxRoutes       = 32;
    static constexpr int MaxLearnedSsrcs = 64;

    struct Route {
        ContentKey    content;
        QByteArray    mid;
        quint16       midExtensionId = 0; // 0 when MID was not negotiated for this content.
        QSet<quint32> incomingSsrcs;
        QSet<quint32> localSsrcs; // Peer RTCP can refer to our local media source.
    };

    struct RoutedPacket {
        ContentKey          content;
        QByteArray          data;
        SrtpContext::Packet kind     = SrtpContext::Packet::Rtp;
        quint64             revision = 0;
    };

    enum class Error { None, InvalidRoutes, MalformedPacket, UnknownRoute, AmbiguousRoute };

    // Transactional: a rejected table leaves the previous routes and revision intact.
    bool configure(const QList<Route> &routes);
    void reset();

    std::optional<RoutedPacket> routeIncoming(const QByteArray &packet, SrtpContext::Packet kind);
    bool                        isCurrent(const RoutedPacket &packet) const;

    quint64 revision() const { return revision_; }
    Error   lastError() const { return lastError_; }
    int     learnedSsrcCount() const { return learnedSsrcs_.size(); }

private:
    struct ParsedRtp {
        quint32                   ssrc = 0;
        std::optional<QByteArray> mid;
    };

    static quint16 read16(const QByteArray &data, int offset);
    static quint32 read32(const QByteArray &data, int offset);
    static void    noteSsrc(const QHash<quint32, int> &mapping, quint32 ssrc, QSet<int> &routes);

    std::optional<ParsedRtp> parseRtp(const QByteArray &packet) const;
    bool                     collectRtcpRoutes(const QByteArray &packet, QSet<int> &routes) const;
    std::optional<RoutedPacket> routed(int routeIndex, const QByteArray &packet, SrtpContext::Packet kind);
    void                        advanceRevision();

    QList<Route>           routes_;
    QMap<ContentKey, int>  contentRoutes_;
    QHash<QByteArray, int> midRoutes_;
    // Incoming and local SSRCs are deliberately separate. Incoming RTP may only
    // use peer SSRCs; local SSRCs are valid only in RTCP report/media-source fields.
    QHash<quint32, int> incomingSsrcRoutes_;
    QHash<quint32, int> localSsrcRoutes_;
    QSet<quint32>       learnedSsrcs_;
    quint16             midExtensionId_ = 0;
    quint64             revision_       = 0;
    Error               lastError_      = Error::None;
};

}

#endif // JINGLE_RTP_ROUTER_P_H
