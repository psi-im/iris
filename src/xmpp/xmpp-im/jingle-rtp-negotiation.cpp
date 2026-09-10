// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp-negotiation.h"

namespace XMPP::Jingle::RTP {
namespace {
    // Validate programmatically constructed descriptions as well as parsed XML.
    // The XML roundtrip also detaches the implicitly shared extension DOM nodes.
    std::optional<Description> snapshot(const Description &description)
    {
        if (description.rtcpMux) {
            for (const auto &payload : description.payloads)
                if (payload.id >= 64 && payload.id <= 95)
                    return {}; // RFC 5761 section 4: RTP/RTCP demultiplexing conflict.
        }
        QDomDocument doc;
        return Description::fromXml(description.toXml(doc));
    }

    bool compatible(const Description &offer, const Description &answer)
    {
        if (offer.media != answer.media || (answer.rtcpMux && !offer.rtcpMux))
            return false;
        for (const auto &selected : answer.payloads) {
            const PayloadType *offered = nullptr;
            for (const auto &candidate : offer.payloads) {
                if (candidate.id == selected.id) {
                    offered = &candidate;
                    break;
                }
            }
            if (!offered)
                return false;
            // Static payloads may omit codec metadata. The media adapter resolves
            // those assignments. Dynamic payloads must identify the same codec.
            if (!offered->name.isEmpty() && !selected.name.isEmpty()
                && offered->name.compare(selected.name, Qt::CaseInsensitive) != 0)
                return false;
            if (offered->clockrate && selected.clockrate && offered->clockrate != selected.clockrate)
                return false;
            if (offered->channels.value_or(1) != selected.channels.value_or(1))
                return false;
        }
        return true;
    }
}

Negotiation::Result Negotiation::setLocalOffer(const Description &offer)
{
    if (state_ != State::Empty)
        return Result::WrongState;
    auto value = snapshot(offer);
    if (!value)
        return Result::InvalidDescription;
    local_ = std::move(value);
    state_ = State::Offered;
    return Result::Ok;
}

Negotiation::Result Negotiation::setRemoteOffer(const Description &offer, const CodecNegotiator &codecs)
{
    if (state_ != State::Empty)
        return Result::WrongState;
    auto value = snapshot(offer);
    if (!value)
        return Result::InvalidDescription;
    // Give the adapter its own DOM copy, not the snapshot being committed.
    auto proposed = codecs.makeAnswer(*snapshot(*value));
    if (!proposed)
        return Result::UnsupportedMedia;
    auto answer = snapshot(*proposed);
    if (!answer)
        return Result::InvalidDescription;
    if (!compatible(*value, *answer))
        return Result::IncompatibleAnswer;
    remote_ = std::move(value);
    local_  = std::move(answer);
    state_  = State::Accepted;
    return Result::Ok;
}

Negotiation::Result Negotiation::setRemoteAnswer(const Description &description, const CodecNegotiator &codecs)
{
    if (state_ != State::Offered)
        return Result::WrongState;
    auto answer = snapshot(description);
    if (!answer)
        return Result::InvalidDescription;
    if (!compatible(*local_, *answer))
        return Result::IncompatibleAnswer;
    if (!codecs.acceptsAnswer(*snapshot(*local_), *snapshot(*answer)))
        return Result::UnsupportedMedia;
    remote_ = std::move(answer);
    state_  = State::Accepted;
    return Result::Ok;
}

std::optional<Description> Negotiation::localDescription() const { return local_ ? snapshot(*local_) : std::nullopt; }

std::optional<Description> Negotiation::remoteDescription() const
{
    return remote_ ? snapshot(*remote_) : std::nullopt;
}
}
