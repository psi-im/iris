// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <iris/jingle-rtp-negotiation.h>
using namespace XMPP::Jingle::RTP;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}
using Result = Negotiation::Result;

class MockCodecs : public CodecNegotiator {
public:
    mutable int                validations = 0;
    bool                       accept      = true;
    bool                       mutateInput = false;
    std::optional<Description> response;
    std::optional<Description> makeAnswer(const Description &offer) const override
    {
        if (mutateInput && !offer.extensions.isEmpty()) {
            auto extension = offer.extensions.first();
            extension.setAttribute("value", "mutated-by-adapter");
        }
        return response;
    }
    bool acceptsAnswer(const Description &offer, const Description &) const override
    {
        ++validations;
        if (mutateInput && !offer.extensions.isEmpty()) {
            auto extension = offer.extensions.first();
            extension.setAttribute("value", "mutated-by-adapter");
        }
        return accept;
    }
};

static Description makeOffer()
{
    Description result;
    result.media   = "audio";
    result.rtcpMux = true;
    PayloadType opus;
    opus.id        = 111;
    opus.name      = "opus";
    opus.clockrate = 48000;
    opus.channels  = 2;
    opus.parameters.insert("useinbandfec", "1");
    PayloadType pcmu;
    pcmu.id         = 0;
    result.payloads = { opus, pcmu };
    QDomDocument doc;
    auto         extension = doc.createElementNS("urn:iris:test", "test");
    extension.setAttribute("value", "original");
    result.extensions.append(extension);
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    auto             offer = makeOffer();
    for (int pt : { 64, 72, 95 }) {
        auto conflict                = makeOffer();
        conflict.payloads.first().id = pt;
        Negotiation muxed;
        check(muxed.setLocalOffer(conflict) == Result::InvalidDescription, "ambiguous mux payload offered");
        conflict.rtcpMux = false;
        Negotiation separate;
        check(separate.setLocalOffer(conflict) == Result::Ok, "mux restriction leaked into separate components");
    }
    auto answer = makeOffer();
    answer.payloads.removeLast();
    answer.payloads.first().parameters.insert("minptime", "10");
    answer.payloads.first().name = "OPUS";
    answer.ssrc                  = 42; // Peer SSRC is independent.
    MockCodecs  codecs;
    Negotiation initiator;
    check(initiator.setRemoteAnswer(answer, codecs) == Result::WrongState, "unsolicited answer accepted");
    check(initiator.setLocalOffer(offer) == Result::Ok, "local offer rejected");
    check(initiator.setLocalOffer(offer) == Result::WrongState, "outstanding offer replaced");
    offer.extensions.first().setAttribute("value", "changed-by-caller");
    check(initiator.localDescription()->extensions.first().attribute("value") == "original",
          "caller changed stored offer");
    auto copy = initiator.localDescription();
    copy->extensions.first().setAttribute("value", "changed-by-getter");
    check(initiator.localDescription()->extensions.first().attribute("value") == "original",
          "getter leaked mutable DOM");

    for (int kind = 0; kind < 7; ++kind) {
        auto invalid = answer;
        if (kind == 0)
            invalid.media = "video";
        if (kind == 1)
            invalid.payloads.first().id = 112;
        if (kind == 2)
            invalid.payloads.first().name = "VP8";
        if (kind == 3)
            invalid.payloads.first().clockrate = 16000;
        if (kind == 4)
            invalid.payloads.first().channels = 1;
        if (kind == 5)
            invalid.payloads.clear();
        if (kind == 6)
            invalid.payloads.append(invalid.payloads.first());
        check(initiator.setRemoteAnswer(invalid, codecs) != Result::Ok, "invalid answer accepted");
        check(initiator.state() == Negotiation::State::Offered && !initiator.remoteDescription(),
              "invalid answer changed committed state");
        check(codecs.validations == 0, "malformed answer reached media adapter");
    }
    codecs.accept = false;
    check(initiator.setRemoteAnswer(answer, codecs) == Result::UnsupportedMedia, "media veto ignored");
    check(initiator.state() == Negotiation::State::Offered, "media veto consumed offer");
    codecs.accept      = true;
    codecs.mutateInput = true;
    check(initiator.setRemoteAnswer(answer, codecs) == Result::Ok, "valid codec-specific answer rejected");
    check(initiator.localDescription()->extensions.first().attribute("value") == "original",
          "adapter changed stored offer");
    check(initiator.remoteDescription()->payloads.first().parameters.value("minptime") == "10",
          "codec-specific answer parameters discarded");
    check(initiator.setRemoteAnswer(answer, codecs) == Result::WrongState, "second answer accepted");

    Negotiation responder;
    check(responder.setRemoteOffer(makeOffer(), codecs) == Result::UnsupportedMedia, "missing codec accepted");
    check(responder.state() == Negotiation::State::Empty, "failed offer committed");
    codecs.response                      = answer;
    codecs.response->payloads.first().id = 112;
    check(responder.setRemoteOffer(makeOffer(), codecs) == Result::IncompatibleAnswer,
          "adapter invented an unoffered payload");
    check(!responder.localDescription() && !responder.remoteDescription(), "invalid local answer committed");
    codecs.response = answer;
    check(responder.setRemoteOffer(makeOffer(), codecs) == Result::Ok, "responder negotiation failed");
    check(responder.remoteDescription()->extensions.first().attribute("value") == "original",
          "answer factory mutated stored remote offer");
    check(responder.localDescription()->ssrc == 42, "local answer not stored separately");

    auto noMux    = makeOffer();
    noMux.rtcpMux = false;
    Negotiation unsolicitedMux;
    check(unsolicitedMux.setLocalOffer(noMux) == Result::Ok, "unmuxed offer rejected");
    check(unsolicitedMux.setRemoteAnswer(answer, codecs) == Result::IncompatibleAnswer, "unsolicited mux accepted");
    Negotiation declinedMux;
    check(declinedMux.setLocalOffer(makeOffer()) == Result::Ok, "mux offer rejected");
    answer.rtcpMux = false;
    check(declinedMux.setRemoteAnswer(answer, codecs) == Result::Ok, "mux refusal rejected");

    Negotiation reordered;
    check(reordered.setLocalOffer(makeOffer()) == Result::Ok, "offer rejected");
    auto reorderedAnswer = makeOffer();
    reorderedAnswer.payloads.swapItemsAt(0, 1);
    check(reordered.setRemoteAnswer(reorderedAnswer, codecs) == Result::Ok, "peer codec preferences rejected");
    check(reordered.remoteDescription()->payloads.first().id == 0, "peer codec preference order lost");
    Negotiation invalidOffer;
    auto        empty = makeOffer();
    empty.payloads.clear();
    check(invalidOffer.setLocalOffer(empty) == Result::InvalidDescription, "empty offer accepted");
    check(invalidOffer.state() == Negotiation::State::Empty, "invalid offer changed state");
    qInfo("RTP negotiation regressions passed");
}
