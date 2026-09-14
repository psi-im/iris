from pathlib import Path

ROOT = Path("target")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"{label}: anchor not found")
    return text.replace(old, new, 1)


def remove_between(text: str, start: str, end: str, label: str) -> str:
    a = text.find(start)
    b = text.find(end, a + len(start)) if a >= 0 else -1
    if a < 0 or b < 0:
        raise SystemExit(f"{label}: range not found")
    return text[:a] + text[b:]


def ensure_newline(path: Path) -> None:
    data = path.read_text()
    path.write_text(data.rstrip("\n") + "\n")
    if not path.read_bytes().endswith(b"\n"):
        raise SystemExit(f"missing final newline: {path}")


# ---------------------------------------------------------------------------
# Shared content-modify fixture: replace the old Session-private collision
# helpers with the public TieBreaker transaction API.
# ---------------------------------------------------------------------------
race = ROOT / "tests/jingle/contentmodifyrace.cpp"
text = race.read_text()
text = replace_once(text, "#include <QtCrypto>\n", "#include <QtCrypto>\n#include <initializer_list>\n", "initializer_list include")

old_payload = '''static QDomElement payload(const J::OutgoingUpdate &update)\n{\n    QDomDocument doc;\n    auto         jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));\n    for (const auto &element : std::get<0>(update)) {\n        auto content = doc.createElementNS(J::NS, QStringLiteral("content"));\n        for (const auto &name : { QStringLiteral("creator"), QStringLiteral("name"), QStringLiteral("senders") }) {\n            if (element.hasAttribute(name))\n                content.setAttribute(name, element.attribute(name));\n        }\n        jingle.appendChild(content);\n    }\n    doc.appendChild(jingle);\n    return jingle;\n}\n'''
new_payload = '''static QDomElement payload(std::initializer_list<const J::OutgoingUpdate *> updates)\n{\n    QDomDocument doc;\n    auto         jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));\n    for (const auto *update : updates) {\n        check(update, "null outgoing update");\n        for (const auto &element : std::get<0>(*update)) {\n            if (element.tagName() != QLatin1String("content"))\n                continue;\n            auto content = doc.createElementNS(J::NS, QStringLiteral("content"));\n            for (const auto &name : { QStringLiteral("creator"), QStringLiteral("name"), QStringLiteral("senders") }) {\n                if (element.hasAttribute(name))\n                    content.setAttribute(name, element.attribute(name));\n            }\n            jingle.appendChild(content);\n        }\n    }\n    doc.appendChild(jingle);\n    return jingle;\n}\n\nstatic QDomElement payload(const J::OutgoingUpdate &update) { return payload({ &update }); }\n'''
text = replace_once(text, old_payload, new_payload, "payload helper")

ack_anchor = '''static void acknowledge(const J::OutgoingUpdate &update, Task *result)\n{\n    const auto &callback = std::get<1>(update);\n    check(bool(callback), "outgoing update has no ACK callback");\n    callback(result);\n}\n\n'''
ack_new = ack_anchor + '''static quint64 startContentModify(J::Session &session,\n                                  std::initializer_list<const J::OutgoingUpdate *> updates)\n{\n    return session.tieBreaker()->outgoingStarted(J::Action::ContentModify, payload(updates));\n}\n\nstatic std::optional<Stanza::Error> failedIqError()\n{\n    return Stanza::Error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::Conflict);\n}\n\nstatic void finishContentModify(J::Session &session, quint64 transaction, bool success, Task *task,\n                                std::initializer_list<const J::OutgoingUpdate *> updates)\n{\n    session.tieBreaker()->outgoingFinished(transaction, success ? std::optional<Stanza::Error>() : failedIqError());\n    for (const auto *update : updates)\n        acknowledge(*update, task);\n    session.tieBreaker()->outgoingCallbacksFinished(transaction);\n}\n\n'''
text = replace_once(text, ack_anchor, ack_new, "transaction helpers")

text = remove_between(text, "static void testGenericResolver()\n", "static void crossedContentModify", "old generic resolver test")

old_cross_start = '''    auto initiatorUpdate = initiatorApp->takeOutgoingUpdate();\n    auto responderUpdate = responderApp->takeOutgoingUpdate();\n    initiator.outgoingActionStarted(J::Action::ContentModify);\n    responder.outgoingActionStarted(J::Action::ContentModify);\n\n    check(initiator.shouldTieBreakIncoming(J::Action::ContentModify),\n          "initiator dispatcher did not reject crossed content-modify");\n    check(!responder.shouldTieBreakIncoming(J::Action::ContentModify),\n          "responder incorrectly rejected initiator content-modify");\n    check(!initiator.shouldTieBreakIncoming(J::Action::TransportInfo),\n          "content-modify collision leaked into unrelated action");\n\n    check(responder.updateFromXml(J::Action::ContentModify, payload(initiatorUpdate)),\n          "responder rejected initiator content-modify");\n'''
new_cross_start = '''    auto initiatorUpdate = initiatorApp->takeOutgoingUpdate();\n    auto responderUpdate = responderApp->takeOutgoingUpdate();\n    const auto initiatorTransaction = startContentModify(initiator, { &initiatorUpdate });\n    const auto responderTransaction = startContentModify(responder, { &responderUpdate });\n\n    const auto initiatorResolution\n        = initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(responderUpdate));\n    const auto responderResolution\n        = responder.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(initiatorUpdate));\n    check(initiatorResolution.solution == J::TieBreaker::Solution::Break,\n          "initiator resolver did not reject crossed content-modify");\n    check(responderResolution.solution == J::TieBreaker::Solution::Postpone && responderResolution.id != 0,\n          "responder resolver did not postpone its losing local intent");\n    check(initiator.tieBreaker()->resolveIncoming(J::Action::TransportInfo, payload(responderUpdate)).solution\n              == J::TieBreaker::Solution::Continue,\n          "content-modify collision leaked into unrelated action");\n\n    check(responder.updateFromXml(J::Action::ContentModify, payload(initiatorUpdate)),\n          "responder rejected initiator content-modify");\n    responder.tieBreaker()->incomingFinished(responderResolution.id, J::TieBreaker::RemoteResult::Applied);\n'''
text = replace_once(text, old_cross_start, new_cross_start, "crossed resolver start")

old_order = '''    if (responderResultFirst) {\n        responder.outgoingActionFinished(J::Action::ContentModify);\n        acknowledge(responderUpdate, failure);\n        initiator.outgoingActionFinished(J::Action::ContentModify);\n        acknowledge(initiatorUpdate, success);\n    } else {\n        initiator.outgoingActionFinished(J::Action::ContentModify);\n        acknowledge(initiatorUpdate, success);\n        responder.outgoingActionFinished(J::Action::ContentModify);\n        acknowledge(responderUpdate, failure);\n    }\n'''
new_order = '''    if (responderResultFirst) {\n        finishContentModify(responder, responderTransaction, false, failure, { &responderUpdate });\n        finishContentModify(initiator, initiatorTransaction, true, success, { &initiatorUpdate });\n    } else {\n        finishContentModify(initiator, initiatorTransaction, true, success, { &initiatorUpdate });\n        finishContentModify(responder, responderTransaction, false, failure, { &responderUpdate });\n    }\n'''
text = replace_once(text, old_order, new_order, "crossed completion ordering")

old_cross_end = '''    check(initiatorApp->senders() == initiatorTarget && responderApp->senders() == initiatorTarget,\n          "crossed content-modify did not converge to initiator state");\n    check(!initiator.shouldTieBreakIncoming(J::Action::ContentModify)\n              && !responder.shouldTieBreakIncoming(J::Action::ContentModify),\n          "crossed content-modify left collision state in flight");\n'''
new_cross_end = '''    check(initiatorApp->senders() == initiatorTarget && responderApp->senders() == initiatorTarget,\n          "crossed content-modify did not converge to initiator state");\n    check(initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(responderUpdate)).solution\n                  == J::TieBreaker::Solution::Continue\n              && responder.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(initiatorUpdate)).solution\n                  == J::TieBreaker::Solution::Continue,\n          "crossed content-modify left collision state in flight");\n    check(responderApp->evaluateOutgoingUpdate().action\n              == (responderTarget == initiatorTarget ? J::Action::NoAction : J::Action::ContentModify),\n          "postponed responder intent was not reconciled after the losing IQ failed");\n'''
text = replace_once(text, old_cross_end, new_cross_end, "crossed final state")

text = replace_once(text, "    testGenericResolver();\n\n", "", "remove old generic test invocation")

old_multi = '''        auto audioUpdate = audio->takeOutgoingUpdate();\n        auto videoUpdate = video->takeOutgoingUpdate();\n        initiator.outgoingActionStarted(J::Action::ContentModify);\n        check(initiator.shouldTieBreakIncoming(J::Action::ContentModify),\n              "multi-content collision did not enable resolver");\n        delete video;\n        std::get<1>(videoUpdate) = {};\n        check(initiator.shouldTieBreakIncoming(J::Action::ContentModify),\n              "removed Application prematurely cleared the still-pending IQ collision");\n        initiator.outgoingActionFinished(J::Action::ContentModify);\n        check(!initiator.shouldTieBreakIncoming(J::Action::ContentModify),\n              "completed multi-content IQ left stale collision state");\n        acknowledge(audioUpdate, &success);\n'''
new_multi = '''        auto audioUpdate = audio->takeOutgoingUpdate();\n        auto videoUpdate = video->takeOutgoingUpdate();\n        const auto transaction = startContentModify(initiator, { &audioUpdate, &videoUpdate });\n        check(initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(audioUpdate)).solution\n                  == J::TieBreaker::Solution::Break,\n              "multi-content collision did not enable resolver");\n        delete video;\n        std::get<1>(videoUpdate) = {};\n        check(initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(audioUpdate)).solution\n                  == J::TieBreaker::Solution::Break,\n              "removed Application prematurely cleared the still-pending IQ collision");\n        initiator.tieBreaker()->outgoingFinished(transaction, std::nullopt);\n        check(initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(audioUpdate)).solution\n                  == J::TieBreaker::Solution::Continue,\n              "completed multi-content IQ left stale collision state");\n        acknowledge(audioUpdate, &success);\n        initiator.tieBreaker()->outgoingCallbacksFinished(transaction);\n'''
text = replace_once(text, old_multi, new_multi, "race multi-content transaction")
race.write_text(text)

# ---------------------------------------------------------------------------
# IQ-boundary regression: production ordering is outgoingFinished -> owner ACK
# callbacks -> outgoingCallbacksFinished.
# ---------------------------------------------------------------------------
boundary = ROOT / "tests/jingle/contentmodifyiqboundary.cpp"
text = boundary.read_text()
text = replace_once(
    text,
    '''        staleTieBreak = session.shouldTieBreakIncoming(J::Action::ContentModify);\n''',
    '''        staleTieBreak = session.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(update)).solution\n            != J::TieBreaker::Solution::Continue;\n''',
    "boundary reentrant resolver",
)
text = replace_once(
    text,
    '''    auto update = content->takeOutgoingUpdate();\n    acknowledge(update, &success);\n\n''',
    '''    auto update      = content->takeOutgoingUpdate();\n    const auto transaction = startContentModify(session, { &update });\n    session.tieBreaker()->outgoingFinished(transaction, std::nullopt);\n    acknowledge(update, &success);\n    session.tieBreaker()->outgoingCallbacksFinished(transaction);\n\n''',
    "boundary transaction ordering",
)
text = replace_once(
    text,
    '''    check(!session.shouldTieBreakIncoming(J::Action::ContentModify),\n          "completed content-modify left stale collision state after its callback");\n''',
    '''    check(session.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(update)).solution\n              == J::TieBreaker::Solution::Continue,\n          "completed content-modify left stale collision state after its callback");\n''',
    "boundary final collision check",
)
boundary.write_text(text)

# ---------------------------------------------------------------------------
# Dispatcher regression: JTPush drives resolveIncoming/incomingFinished. Tests
# only synthesize the local outgoing transaction around Application callbacks.
# ---------------------------------------------------------------------------
dispatcher = ROOT / "tests/jingle/contentmodifydispatcher.cpp"
text = dispatcher.read_text()
old_complete = '''static void completeCrossed(J::Session &initiator, J::Session &responder, const J::OutgoingUpdate &initiatorUpdate,\n                            const J::OutgoingUpdate &responderUpdate, Task *success, Task *failure,\n                            bool responderResultFirst)\n{\n    if (responderResultFirst) {\n        responder.outgoingActionFinished(J::Action::ContentModify);\n        acknowledge(responderUpdate, failure);\n        initiator.outgoingActionFinished(J::Action::ContentModify);\n        acknowledge(initiatorUpdate, success);\n    } else {\n        initiator.outgoingActionFinished(J::Action::ContentModify);\n        acknowledge(initiatorUpdate, success);\n        responder.outgoingActionFinished(J::Action::ContentModify);\n        acknowledge(responderUpdate, failure);\n    }\n}\n'''
new_complete = '''static void completeCrossed(J::Session &initiator, J::Session &responder, quint64 initiatorTransaction,\n                            quint64 responderTransaction, const J::OutgoingUpdate &initiatorUpdate,\n                            const J::OutgoingUpdate &responderUpdate, Task *success, Task *failure,\n                            bool responderResultFirst)\n{\n    if (responderResultFirst) {\n        finishContentModify(responder, responderTransaction, false, failure, { &responderUpdate });\n        finishContentModify(initiator, initiatorTransaction, true, success, { &initiatorUpdate });\n    } else {\n        finishContentModify(initiator, initiatorTransaction, true, success, { &initiatorUpdate });\n        finishContentModify(responder, responderTransaction, false, failure, { &responderUpdate });\n    }\n}\n'''
text = replace_once(text, old_complete, new_complete, "dispatcher completion helper")

text = replace_once(
    text,
    '''    auto initiatorUpdate = initiatorApp->takeOutgoingUpdate();\n    auto responderUpdate = responderApp->takeOutgoingUpdate();\n    initiator.outgoingActionStarted(J::Action::ContentModify);\n    responder.outgoingActionStarted(J::Action::ContentModify);\n\n''',
    '''    auto initiatorUpdate = initiatorApp->takeOutgoingUpdate();\n    auto responderUpdate = responderApp->takeOutgoingUpdate();\n    const auto initiatorTransaction = startContentModify(initiator, { &initiatorUpdate });\n    const auto responderTransaction = startContentModify(responder, { &responderUpdate });\n\n''',
    "dispatcher crossed start",
)
text = replace_once(
    text,
    '''    completeCrossed(initiator, responder, initiatorUpdate, responderUpdate, &success, &failure,\n                    responderResultFirst);\n\n''',
    '''    completeCrossed(initiator, responder, initiatorTransaction, responderTransaction, initiatorUpdate,\n                    responderUpdate, &success, &failure, responderResultFirst);\n\n''',
    "dispatcher crossed finish call",
)
text = replace_once(
    text,
    '''    check(!initiator.shouldTieBreakIncoming(J::Action::ContentModify)\n              && !responder.shouldTieBreakIncoming(J::Action::ContentModify),\n          "dispatcher-level crossed content-modify left collision state active");\n''',
    '''    check(initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(responderUpdate)).solution\n                  == J::TieBreaker::Solution::Continue\n              && responder.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(initiatorUpdate)).solution\n                  == J::TieBreaker::Solution::Continue,\n          "dispatcher-level crossed content-modify left collision state active");\n''',
    "dispatcher crossed final collision",
)

text = replace_once(
    text,
    '''    auto iau = ia->takeOutgoingUpdate();\n    auto ivu = iv->takeOutgoingUpdate();\n    auto rau = ra->takeOutgoingUpdate();\n    auto rvu = rv->takeOutgoingUpdate();\n    initiator.outgoingActionStarted(J::Action::ContentModify);\n    responder.outgoingActionStarted(J::Action::ContentModify);\n\n''',
    '''    auto iau = ia->takeOutgoingUpdate();\n    auto ivu = iv->takeOutgoingUpdate();\n    auto rau = ra->takeOutgoingUpdate();\n    auto rvu = rv->takeOutgoingUpdate();\n    const auto initiatorTransaction = startContentModify(initiator, { &iau, &ivu });\n    const auto responderTransaction = startContentModify(responder, { &rau, &rvu });\n\n''',
    "dispatcher multi start",
)
text = replace_once(
    text,
    '''    responder.outgoingActionFinished(J::Action::ContentModify);\n    acknowledge(rau, &failure);\n    acknowledge(rvu, &failure);\n    initiator.outgoingActionFinished(J::Action::ContentModify);\n    acknowledge(iau, &success);\n    acknowledge(ivu, &success);\n''',
    '''    finishContentModify(responder, responderTransaction, false, &failure, { &rau, &rvu });\n    finishContentModify(initiator, initiatorTransaction, true, &success, { &iau, &ivu });\n''',
    "dispatcher multi completion",
)

text = replace_once(
    text,
    '''    initiator.outgoingActionStarted(J::Action::ContentModify);\n    responder.outgoingActionStarted(J::Action::ContentModify);\n\n    // Session::doStep stores callbacks behind QPointer<Application>. Clearing the\n''',
    '''    const auto initiatorTransaction = startContentModify(initiator, { &iau, &ivu });\n    const auto responderTransaction = startContentModify(responder, { &rau, &rvu });\n\n    // Session::doStep stores callbacks behind QPointer<Application>. Clearing the\n''',
    "dispatcher deletion start",
)
text = replace_once(
    text,
    '''    check(initiator.shouldTieBreakIncoming(J::Action::ContentModify),\n          "content deletion incorrectly ended the Session-level IQ collision");\n''',
    '''    check(initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(rau)).solution\n              == J::TieBreaker::Solution::Break,\n          "content deletion incorrectly ended the Session-level IQ collision");\n''',
    "dispatcher deletion active collision",
)
text = replace_once(
    text,
    '''    responder.outgoingActionFinished(J::Action::ContentModify);\n    acknowledge(rau, &failure);\n    acknowledge(rvu, &failure);\n    initiator.outgoingActionFinished(J::Action::ContentModify);\n    acknowledge(iau, &success);\n\n''',
    '''    finishContentModify(responder, responderTransaction, false, &failure, { &rau, &rvu });\n    finishContentModify(initiator, initiatorTransaction, true, &success, { &iau });\n\n''',
    "dispatcher deletion completion",
)
text = replace_once(
    text,
    '''    check(!initiator.shouldTieBreakIncoming(J::Action::ContentModify),\n          "completed IQ kept collision active after sibling deletion");\n''',
    '''    check(initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(rau)).solution\n              == J::TieBreaker::Solution::Continue,\n          "completed IQ kept collision active after sibling deletion");\n''',
    "dispatcher deletion final collision",
)

text = replace_once(
    text,
    '''    auto update = content->takeOutgoingUpdate();\n    session.outgoingActionStarted(J::Action::ContentModify);\n\n''',
    '''    auto update = content->takeOutgoingUpdate();\n    const auto transaction = startContentModify(session, { &update });\n\n''',
    "dispatcher post-IQ start",
)
text = replace_once(
    text,
    '''    session.outgoingActionFinished(J::Action::ContentModify);\n    acknowledge(update, &success);\n\n''',
    '''    session.tieBreaker()->outgoingFinished(transaction, std::nullopt);\n    acknowledge(update, &success);\n    session.tieBreaker()->outgoingCallbacksFinished(transaction);\n\n''',
    "dispatcher post-IQ completion",
)
dispatcher.write_text(text)

for path in [race, boundary, dispatcher]:
    ensure_newline(path)
