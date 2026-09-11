// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp-router_p.h"

namespace XMPP::Jingle::RTP {
namespace {
    constexpr int MaxConfiguredSsrcs = 256;
}

quint16 BundleRouter::read16(const QByteArray &data, int offset)
{
    const auto *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return quint16((quint16(p[0]) << 8) | quint16(p[1]));
}

quint32 BundleRouter::read32(const QByteArray &data, int offset)
{
    const auto *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

void BundleRouter::noteSsrc(const QHash<quint32, int> &mapping, quint32 ssrc, QSet<int> &routes)
{
    if (!ssrc)
        return; // zero has protocol-specific wildcard meanings in RTCP feedback.
    const auto it = mapping.constFind(ssrc);
    if (it != mapping.cend())
        routes.insert(it.value());
}

void BundleRouter::advanceRevision()
{
    ++revision_;
    if (!revision_)
        ++revision_;
}

bool BundleRouter::configure(const QList<Route> &routes)
{
    if (routes.isEmpty() || routes.size() > MaxRoutes) {
        lastError_ = Error::InvalidRoutes;
        return false;
    }

    QMap<ContentKey, int>  contentRoutes;
    QHash<QByteArray, int> midRoutes;
    QHash<quint8, int>     payloadTypeRoutes;
    QSet<quint8>           ambiguousPayloadTypes;
    QHash<quint32, int>    incomingSsrcRoutes;
    QHash<quint32, int>    localSsrcRoutes;
    QSet<QByteArray>       mids;
    quint16                midExtensionId = 0;

    auto addSsrc = [](QHash<quint32, int> &mapping, quint32 ssrc, int routeIndex) {
        auto it = mapping.constFind(ssrc);
        if (it != mapping.cend() && it.value() != routeIndex)
            return false;
        mapping.insert(ssrc, routeIndex);
        return true;
    };

    for (int routeIndex = 0; routeIndex < routes.size(); ++routeIndex) {
        const auto &route = routes.at(routeIndex);
        if (route.content.first.isEmpty()
            || (route.content.second != Origin::Initiator && route.content.second != Origin::Responder)
            || contentRoutes.contains(route.content)) {
            lastError_ = Error::InvalidRoutes;
            return false;
        }
        contentRoutes.insert(route.content, routeIndex);

        if (!route.mid.isEmpty()) {
            if (mids.contains(route.mid)) {
                lastError_ = Error::InvalidRoutes;
                return false;
            }
            mids.insert(route.mid);
        }
        if (route.midExtensionId) {
            // 256 is the RFC 8285 appbits signaling value, not an RTP extension
            // element id. Extended offer-only ids are likewise unusable on wire.
            if (route.midExtensionId > 255 || route.mid.isEmpty()
                || (midExtensionId && midExtensionId != route.midExtensionId)) {
                lastError_ = Error::InvalidRoutes;
                return false;
            }
            midExtensionId = route.midExtensionId;
            midRoutes.insert(route.mid, routeIndex);
        }

        for (auto payloadType : route.incomingPayloadTypes) {
            if (payloadType > 127) {
                lastError_ = Error::InvalidRoutes;
                return false;
            }
            if (ambiguousPayloadTypes.contains(payloadType))
                continue;
            auto existing = payloadTypeRoutes.constFind(payloadType);
            if (existing == payloadTypeRoutes.cend()) {
                payloadTypeRoutes.insert(payloadType, routeIndex);
            } else if (existing.value() != routeIndex) {
                payloadTypeRoutes.remove(payloadType);
                ambiguousPayloadTypes.insert(payloadType);
            }
        }

        for (auto ssrc : route.incomingSsrcs) {
            if (!addSsrc(incomingSsrcRoutes, ssrc, routeIndex)) {
                lastError_ = Error::InvalidRoutes;
                return false;
            }
        }
        for (auto ssrc : route.localSsrcs) {
            if (!addSsrc(localSsrcRoutes, ssrc, routeIndex)) {
                lastError_ = Error::InvalidRoutes;
                return false;
            }
        }
        if (incomingSsrcRoutes.size() + localSsrcRoutes.size() > MaxConfiguredSsrcs) {
            lastError_ = Error::InvalidRoutes;
            return false;
        }
    }

    routes_              = routes;
    contentRoutes_       = std::move(contentRoutes);
    midRoutes_           = std::move(midRoutes);
    payloadTypeRoutes_   = std::move(payloadTypeRoutes);
    incomingSsrcRoutes_  = std::move(incomingSsrcRoutes);
    localSsrcRoutes_     = std::move(localSsrcRoutes);
    learnedSsrcs_.clear();
    midExtensionId_ = midExtensionId;
    advanceRevision();
    lastError_ = Error::None;
    return true;
}

void BundleRouter::reset()
{
    routes_.clear();
    contentRoutes_.clear();
    midRoutes_.clear();
    payloadTypeRoutes_.clear();
    incomingSsrcRoutes_.clear();
    localSsrcRoutes_.clear();
    learnedSsrcs_.clear();
    midExtensionId_ = 0;
    advanceRevision();
    lastError_ = Error::None;
}

std::optional<BundleRouter::ParsedRtp> BundleRouter::parseRtp(const QByteArray &packet) const
{
    if (packet.size() < 12)
        return {};
    const auto *bytes = reinterpret_cast<const uchar *>(packet.constData());
    if ((bytes[0] >> 6) != 2)
        return {};

    const int csrcCount = bytes[0] & 0x0f;
    int       offset    = 12 + csrcCount * 4;
    if (offset > packet.size())
        return {};

    ParsedRtp result;
    result.payloadType = bytes[1] & 0x7f;
    result.ssrc        = read32(packet, 8);
    if (!(bytes[0] & 0x10))
        return result;
    if (offset + 4 > packet.size())
        return {};

    const quint16 profile      = read16(packet, offset);
    const quint16 lengthWords  = read16(packet, offset + 2);
    const int     extensionEnd = offset + 4 + int(lengthWords) * 4;
    if (extensionEnd > packet.size())
        return {};
    if (!midExtensionId_)
        return result;

    int cursor = offset + 4;
    if (profile == 0xbede && midExtensionId_ <= 14) {
        while (cursor < extensionEnd) {
            const quint8 header = quint8(packet.at(cursor++));
            if (!header)
                continue;
            const quint8 id = header >> 4;
            if (id == 15)
                break; // RFC 8285: reserved value terminates extension processing.
            const int length = (header & 0x0f) + 1;
            if (cursor + length > extensionEnd)
                return {};
            if (id == midExtensionId_) {
                if (result.mid)
                    return {};
                result.mid = packet.mid(cursor, length);
                if (result.mid->isEmpty())
                    return {};
            }
            cursor += length;
        }
    } else if ((profile & 0xfff0) == 0x1000) {
        while (cursor < extensionEnd) {
            const quint8 id = quint8(packet.at(cursor++));
            if (!id)
                continue;
            if (cursor >= extensionEnd)
                return {};
            const int length = quint8(packet.at(cursor++));
            if (cursor + length > extensionEnd)
                return {};
            if (id == midExtensionId_) {
                if (result.mid || !length)
                    return {};
                result.mid = packet.mid(cursor, length);
            }
            cursor += length;
        }
    }
    return result;
}

bool BundleRouter::collectRtcpRoutes(const QByteArray &packet, QSet<int> &routes) const
{
    int offset = 0;
    while (offset < packet.size()) {
        if (offset + 4 > packet.size())
            return false;
        const auto *bytes = reinterpret_cast<const uchar *>(packet.constData() + offset);
        if ((bytes[0] >> 6) != 2)
            return false;

        const bool   padding     = bytes[0] & 0x20;
        const int    count       = bytes[0] & 0x1f;
        const quint8 packetType  = bytes[1];
        const int    blockLength = (int(read16(packet, offset + 2)) + 1) * 4;
        if (blockLength < 4 || offset + blockLength > packet.size())
            return false;

        int payloadEnd = offset + blockLength;
        if (padding) {
            if (payloadEnd != packet.size())
                return false; // Only the last packet in a compound RTCP packet may be padded.
            const int paddingLength = quint8(packet.at(payloadEnd - 1));
            if (!paddingLength || paddingLength > blockLength - 4)
                return false;
            payloadEnd -= paddingLength;
        }

        switch (packetType) {
        case 200: { // Sender Report
            if (payloadEnd - offset < 28 + count * 24)
                return false;
            noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4), routes);
            for (int i = 0; i < count; ++i)
                noteSsrc(localSsrcRoutes_, read32(packet, offset + 28 + i * 24), routes);
            break;
        }
        case 201: { // Receiver Report
            if (payloadEnd - offset < 8 + count * 24)
                return false;
            noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4), routes);
            for (int i = 0; i < count; ++i)
                noteSsrc(localSsrcRoutes_, read32(packet, offset + 8 + i * 24), routes);
            break;
        }
        case 202: { // SDES
            int cursor = offset + 4;
            for (int chunk = 0; chunk < count; ++chunk) {
                if (cursor + 4 > payloadEnd)
                    return false;
                noteSsrc(incomingSsrcRoutes_, read32(packet, cursor), routes);
                cursor += 4;
                bool ended = false;
                while (cursor < payloadEnd) {
                    const quint8 type = quint8(packet.at(cursor++));
                    if (!type) {
                        ended = true;
                        break;
                    }
                    if (cursor >= payloadEnd)
                        return false;
                    const int length = quint8(packet.at(cursor++));
                    if (cursor + length > payloadEnd)
                        return false;
                    cursor += length;
                }
                if (!ended)
                    return false;
                while ((cursor - offset) & 3) {
                    if (cursor >= payloadEnd || packet.at(cursor++) != 0)
                        return false;
                }
            }
            if (cursor != payloadEnd)
                return false;
            break;
        }
        case 203: // BYE
            if (payloadEnd - offset < 4 + count * 4)
                return false;
            for (int i = 0; i < count; ++i)
                noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4 + i * 4), routes);
            break;
        case 204: // APP
            if (payloadEnd - offset < 12)
                return false;
            noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4), routes);
            break;
        case 205: // RTPFB
        case 206: // PSFB
            if (payloadEnd - offset < 12)
                return false;
            noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4), routes);
            noteSsrc(localSsrcRoutes_, read32(packet, offset + 8), routes);
            break;
        case 207: // XR
            if (payloadEnd - offset < 8)
                return false;
            noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4), routes);
            break;
        default:
            // Unknown RTCP packet types may contain routing SSRCs we do not know
            // how to interpret. Fail closed instead of attaching them to a content
            // selected by a different block in the same authenticated compound.
            return false;
        }
        offset += blockLength;
    }
    return offset == packet.size();
}

std::optional<BundleRouter::RoutedPacket> BundleRouter::routed(int routeIndex, const QByteArray &packet,
                                                               SrtpContext::Packet kind)
{
    if (routeIndex < 0 || routeIndex >= routes_.size()) {
        lastError_ = Error::UnknownRoute;
        return {};
    }
    lastError_ = Error::None;
    return RoutedPacket { routes_.at(routeIndex).content, packet, kind, revision_ };
}

std::optional<BundleRouter::RoutedPacket> BundleRouter::routeIncoming(const QByteArray &packet,
                                                                      SrtpContext::Packet kind)
{
    if (kind == SrtpContext::Packet::Rtp) {
        auto parsed = parseRtp(packet);
        if (!parsed) {
            lastError_ = Error::MalformedPacket;
            return {};
        }

        auto ssrcRoute    = incomingSsrcRoutes_.constFind(parsed->ssrc);
        auto payloadRoute = payloadTypeRoutes_.constFind(parsed->payloadType);
        if (parsed->mid) {
            auto midRoute = midRoutes_.constFind(*parsed->mid);
            if (midRoute == midRoutes_.cend()) {
                lastError_ = Error::UnknownRoute;
                return {};
            }
            if ((ssrcRoute != incomingSsrcRoutes_.cend() && ssrcRoute.value() != midRoute.value())
                || (payloadRoute != payloadTypeRoutes_.cend() && payloadRoute.value() != midRoute.value())) {
                lastError_ = Error::AmbiguousRoute;
                return {};
            }
            if (ssrcRoute == incomingSsrcRoutes_.cend() && parsed->ssrc
                && learnedSsrcs_.size() < MaxLearnedSsrcs) {
                incomingSsrcRoutes_.insert(parsed->ssrc, midRoute.value());
                learnedSsrcs_.insert(parsed->ssrc);
            }
            return routed(midRoute.value(), packet, kind);
        }

        if (ssrcRoute != incomingSsrcRoutes_.cend()) {
            if (payloadRoute != payloadTypeRoutes_.cend() && payloadRoute.value() != ssrcRoute.value()) {
                lastError_ = Error::AmbiguousRoute;
                return {};
            }
            return routed(ssrcRoute.value(), packet, kind);
        }

        if (payloadRoute != payloadTypeRoutes_.cend()) {
            if (parsed->ssrc && learnedSsrcs_.size() < MaxLearnedSsrcs) {
                incomingSsrcRoutes_.insert(parsed->ssrc, payloadRoute.value());
                learnedSsrcs_.insert(parsed->ssrc);
            }
            return routed(payloadRoute.value(), packet, kind);
        }

        lastError_ = Error::UnknownRoute;
        return {};
    }

    QSet<int> routes;
    if (!collectRtcpRoutes(packet, routes)) {
        lastError_ = Error::MalformedPacket;
        return {};
    }
    if (routes.isEmpty()) {
        lastError_ = Error::UnknownRoute;
        return {};
    }
    if (routes.size() != 1) {
        lastError_ = Error::AmbiguousRoute;
        return {};
    }
    return routed(*routes.cbegin(), packet, kind);
}

bool BundleRouter::isCurrent(const RoutedPacket &packet) const
{
    return packet.revision == revision_ && contentRoutes_.contains(packet.content);
}

}
