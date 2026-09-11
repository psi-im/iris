#include "../../src/xmpp/xmpp-im/jingle-rtp-router_p.h"

#include <QCoreApplication>

using namespace XMPP::Jingle;
using namespace XMPP::Jingle::RTP;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static void write16(QByteArray &data, int offset, quint16 value)
{
    data[offset]     = char(value >> 8);
    data[offset + 1] = char(value);
}

static void write32(QByteArray &data, int offset, quint32 value)
{
    data[offset]     = char(value >> 24);
    data[offset + 1] = char(value >> 16);
    data[offset + 2] = char(value >> 8);
    data[offset + 3] = char(value);
}

static void append16(QByteArray &data, quint16 value)
{
    data.append(char(value >> 8));
    data.append(char(value));
}

static QByteArray rtp(quint32 ssrc, const QByteArray &mid = {}, quint8 extensionId = 1, bool twoByte = false)
{
    QByteArray packet(12, '\0');
    packet[0] = char(0x80 | (mid.isEmpty() ? 0 : 0x10));
    packet[1] = char(111);
    write32(packet, 8, ssrc);
    if (mid.isEmpty())
        return packet;

    QByteArray extensions;
    if (twoByte) {
        extensions.append(char(extensionId));
        extensions.append(char(mid.size()));
        extensions.append(mid);
    } else {
        check(extensionId <= 14 && !mid.isEmpty() && mid.size() <= 16, "invalid one-byte test extension");
        extensions.append(char((extensionId << 4) | (mid.size() - 1)));
        extensions.append(mid);
    }
    while (extensions.size() & 3)
        extensions.append('\0');
    append16(packet, twoByte ? 0x1000 : 0xbede);
    append16(packet, quint16(extensions.size() / 4));
    packet.append(extensions);
    return packet;
}

static QByteArray rtcp(quint8 type, quint8 count, QByteArray payload)
{
    while ((payload.size() + 4) & 3)
        payload.append('\0');
    QByteArray packet(4, '\0');
    packet[0] = char(0x80 | (count & 0x1f));
    packet[1] = char(type);
    packet.append(payload);
    write16(packet, 2, quint16(packet.size() / 4 - 1));
    return packet;
}

static QByteArray senderReport(quint32 sender)
{
    QByteArray payload(24, '\0');
    write32(payload, 0, sender);
    return rtcp(200, 0, payload);
}

static QByteArray receiverReport(quint32 sender, quint32 reported)
{
    QByteArray payload(28, '\0');
    write32(payload, 0, sender);
    write32(payload, 4, reported);
    return rtcp(201, 1, payload);
}

static QByteArray feedback(quint8 type, quint32 sender, quint32 media)
{
    QByteArray payload(8, '\0');
    write32(payload, 0, sender);
    write32(payload, 4, media);
    return rtcp(type, 1, payload);
}

static QByteArray sdes(quint32 ssrc)
{
    QByteArray payload(4, '\0');
    write32(payload, 0, ssrc);
    payload.append(char(1)); // CNAME
    payload.append(char(1));
    payload.append('x');
    payload.append('\0'); // end
    return rtcp(202, 1, payload);
}

static BundleRouter::Route route(const char *name, const char *mid, quint16 midId, quint32 incoming, quint32 local)
{
    BundleRouter::Route result;
    result.content = ContentKey { QString::fromLatin1(name), Origin::Initiator };
    result.mid = QByteArray(mid);
    result.midExtensionId = midId;
    result.incomingSsrcs.insert(incoming);
    result.localSsrcs.insert(local);
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    constexpr quint32 AudioRemote = 0x11111111;
    constexpr quint32 VideoRemote = 0x22222222;
    constexpr quint32 AudioLocal  = 0xaaaaaaaa;
    constexpr quint32 VideoLocal  = 0xbbbbbbbb;

    const auto audio = route("audio", "audio", 1, AudioRemote, AudioLocal);
    const auto video = route("video", "video", 1, VideoRemote, VideoLocal);

    BundleRouter router;
    check(router.configure({ audio, video }), "valid BUNDLE routes rejected");
    const auto initialRevision = router.revision();
    check(initialRevision != 0, "route revision was not initialized");

    auto audioPacket = router.routeIncoming(rtp(AudioRemote), SrtpContext::Packet::Rtp);
    check(audioPacket && audioPacket->content == audio.content && router.isCurrent(*audioPacket),
          "known audio SSRC routed incorrectly");

    const quint32 learnedVideo = 0x33333333;
    auto videoByMid = router.routeIncoming(rtp(learnedVideo, "video"), SrtpContext::Packet::Rtp);
    check(videoByMid && videoByMid->content == video.content && router.learnedSsrcCount() == 1,
          "authenticated MID did not route and learn a new SSRC");
    auto videoByLearnedSsrc = router.routeIncoming(rtp(learnedVideo), SrtpContext::Packet::Rtp);
    check(videoByLearnedSsrc && videoByLearnedSsrc->content == video.content,
          "learned SSRC did not route without repeated MID");

    check(!router.routeIncoming(rtp(AudioRemote, "video"), SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::AmbiguousRoute,
          "MID/SSRC route conflict was not dropped");
    check(!router.routeIncoming(rtp(0x44444444, "missing"), SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::UnknownRoute,
          "unknown explicit MID fell back to an unrelated SSRC route");
    check(!router.routeIncoming(rtp(0x44444444), SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::UnknownRoute,
          "unknown RTP SSRC was guessed");
    check(!router.routeIncoming(rtp(AudioLocal), SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::UnknownRoute,
          "local SSRC was incorrectly accepted as an incoming RTP route");

    auto malformedRtp = rtp(0x55555555, "audio");
    malformedRtp.chop(1);
    check(!router.routeIncoming(malformedRtp, SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::MalformedPacket,
          "truncated RTP extension accepted");

    BundleRouter twoByteRouter;
    auto twoByteAudio = audio;
    twoByteAudio.midExtensionId = 16;
    check(twoByteRouter.configure({ twoByteAudio }), "two-byte MID mapping rejected");
    auto twoBytePacket = twoByteRouter.routeIncoming(rtp(0x66666666, "audio", 16, true), SrtpContext::Packet::Rtp);
    check(twoBytePacket && twoBytePacket->content == audio.content, "two-byte RTP MID was not routed");

    const auto beforeInvalidRevision = router.revision();
    auto duplicateMid = video;
    duplicateMid.mid = audio.mid;
    check(!router.configure({ audio, duplicateMid }) && router.lastError() == BundleRouter::Error::InvalidRoutes
              && router.revision() == beforeInvalidRevision,
          "duplicate MID partially replaced the route table");
    check(bool(router.routeIncoming(rtp(AudioRemote), SrtpContext::Packet::Rtp)),
          "failed configuration destroyed the previous route table");

    auto duplicateSsrc = video;
    duplicateSsrc.incomingSsrcs.clear();
    duplicateSsrc.incomingSsrcs.insert(AudioRemote);
    check(!router.configure({ audio, duplicateSsrc }), "one incoming SSRC was assigned to two contents");
    auto duplicateLocalSsrc = video;
    duplicateLocalSsrc.localSsrcs.clear();
    duplicateLocalSsrc.localSsrcs.insert(AudioLocal);
    check(!router.configure({ audio, duplicateLocalSsrc }), "one local SSRC was assigned to two contents");
    auto differentMidId = video;
    differentMidId.midExtensionId = 2;
    check(!router.configure({ audio, differentMidId }), "different BUNDLE MID extension ids accepted");
    auto appbitsMid = audio;
    appbitsMid.midExtensionId = 256;
    check(!router.configure({ appbitsMid }), "RFC 8285 appbits value used as a MID element id");

    // The same numeric SSRC may exist in opposite directions. RTP uses only the
    // peer/incoming map, while RTCP sender and media-source fields use their
    // protocol-defined direction.
    auto directionalVideo = video;
    directionalVideo.incomingSsrcs.clear();
    directionalVideo.incomingSsrcs.insert(AudioLocal);
    BundleRouter directional;
    check(directional.configure({ audio, directionalVideo }), "cross-direction SSRC collision was rejected");
    auto collidingRtp = directional.routeIncoming(rtp(AudioLocal), SrtpContext::Packet::Rtp);
    check(collidingRtp && collidingRtp->content == video.content,
          "incoming RTP used the local RTCP SSRC namespace");
    auto senderDirected = directional.routeIncoming(feedback(206, AudioLocal, 0), SrtpContext::Packet::Rtcp);
    check(senderDirected && senderDirected->content == video.content,
          "RTCP sender SSRC used the local media-source namespace");
    auto mediaDirected = directional.routeIncoming(feedback(206, 0, AudioLocal), SrtpContext::Packet::Rtcp);
    check(mediaDirected && mediaDirected->content == audio.content,
          "RTCP media SSRC used the incoming sender namespace");

    // Route-level revisions fence queued delivery after membership changes.
    auto queued = router.routeIncoming(rtp(AudioRemote), SrtpContext::Packet::Rtp);
    check(queued && router.isCurrent(*queued), "failed to create queued route regression");
    check(router.configure({ audio }), "valid route table replacement failed");
    check(!router.isCurrent(*queued) && router.revision() != initialRevision && router.learnedSsrcCount() == 0,
          "route replacement did not invalidate stale delivery or learned sources");

    // RTCP is routed by all known sender/media/report SSRCs in the compound packet.
    BundleRouter rtcpRouter;
    check(rtcpRouter.configure({ audio, video }), "RTCP route table rejected");
    auto audioSr = rtcpRouter.routeIncoming(senderReport(AudioRemote), SrtpContext::Packet::Rtcp);
    check(audioSr && audioSr->content == audio.content, "RTCP sender report routed incorrectly");
    auto audioRr = rtcpRouter.routeIncoming(receiverReport(0x77777777, AudioLocal), SrtpContext::Packet::Rtcp);
    check(audioRr && audioRr->content == audio.content, "RTCP receiver report target was not routed");
    auto videoFeedback = rtcpRouter.routeIncoming(feedback(206, 0x88888888, VideoLocal), SrtpContext::Packet::Rtcp);
    check(videoFeedback && videoFeedback->content == video.content, "RTCP feedback media SSRC was not routed");
    auto audioSdes = rtcpRouter.routeIncoming(sdes(AudioRemote), SrtpContext::Packet::Rtcp);
    check(audioSdes && audioSdes->content == audio.content, "RTCP SDES chunk was not routed");

    const auto crossContent = senderReport(AudioRemote) + senderReport(VideoRemote);
    check(!rtcpRouter.routeIncoming(crossContent, SrtpContext::Packet::Rtcp)
              && rtcpRouter.lastError() == BundleRouter::Error::AmbiguousRoute,
          "cross-content compound RTCP packet was broadcast or guessed");
    check(!rtcpRouter.routeIncoming(senderReport(0x99999999), SrtpContext::Packet::Rtcp)
              && rtcpRouter.lastError() == BundleRouter::Error::UnknownRoute,
          "unknown RTCP sender was guessed");
    check(!rtcpRouter.routeIncoming(rtcp(208, 0, QByteArray(4, '\0')), SrtpContext::Packet::Rtcp)
              && rtcpRouter.lastError() == BundleRouter::Error::MalformedPacket,
          "unknown RTCP packet type was attached to another compound route");
    auto malformedRtcp = senderReport(AudioRemote);
    write16(malformedRtcp, 2, 100);
    check(!rtcpRouter.routeIncoming(malformedRtcp, SrtpContext::Packet::Rtcp)
              && rtcpRouter.lastError() == BundleRouter::Error::MalformedPacket,
          "invalid RTCP block length accepted");

    // Learning is bounded. MID still routes after the cap, but an unlearned SSRC
    // cannot later bypass MID-based demultiplexing.
    BundleRouter bounded;
    check(bounded.configure({ audio }), "bounded learning route rejected");
    quint32 last = 0;
    for (int i = 0; i < BundleRouter::MaxLearnedSsrcs + 1; ++i) {
        last = 0x10000000u + quint32(i);
        auto routed = bounded.routeIncoming(rtp(last, "audio"), SrtpContext::Packet::Rtp);
        check(routed && routed->content == audio.content, "MID routing failed while learning SSRCs");
    }
    check(bounded.learnedSsrcCount() == BundleRouter::MaxLearnedSsrcs, "learned SSRC bound was not enforced");
    check(!bounded.routeIncoming(rtp(last), SrtpContext::Packet::Rtp)
              && bounded.lastError() == BundleRouter::Error::UnknownRoute,
          "SSRC beyond the learning bound became an implicit route");

    auto beforeReset = bounded.routeIncoming(rtp(AudioRemote), SrtpContext::Packet::Rtp);
    check(beforeReset && bounded.isCurrent(*beforeReset), "reset revision regression setup failed");
    bounded.reset();
    check(!bounded.isCurrent(*beforeReset)
              && !bounded.routeIncoming(rtp(AudioRemote), SrtpContext::Packet::Rtp)
              && bounded.lastError() == BundleRouter::Error::UnknownRoute,
          "reset retained a stale RTP route");

    qInfo("RTP BUNDLE router regressions passed");
}
