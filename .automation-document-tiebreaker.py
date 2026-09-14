from pathlib import Path

ROOT = Path("target")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"{label}: anchor not found")
    return text.replace(old, new, 1)


def ensure_newline(path: Path) -> None:
    data = path.read_text()
    path.write_text(data.rstrip("\n") + "\n")
    if not path.read_bytes().endswith(b"\n"):
        raise SystemExit(f"missing final newline: {path}")


architecture = ROOT / "docs/jingle.md"
text = architecture.read_text()
section = r'''## Existing-session tie-break coordination

Every incoming Jingle IQ for an already known session passes through the session-owned
`Jingle::TieBreaker` before `Session::updateFromXml()` handles the action. The coordinator is
intentionally generic with respect to application and transport semantics: it only correlates the
currently outstanding local Jingle IQ with incoming actions and dispatches registered resolvers.

A session-bound owner may register any number of resolvers for an action:

```cpp
auto registration = pad->tieBreaker()->registerResolver(Action::ContentModify, resolver);
```

`SessionManagerPad::tieBreaker()` is a convenience for application and transport pads; an
`Application`, `Transport`, selector or another session-owned object may also register directly via
`Session::tieBreaker()`. Registrations are move-only RAII handles, so destroying the owner can
unregister it without a separate lifetime protocol.

A resolver receives the serialized local and remote `<jingle/>` elements and returns one of three
solutions:

| Solution | Meaning |
| --- | --- |
| `Continue` | Tie-break does not intervene. The incoming action follows normal Session parsing and may still succeed or fail for unrelated protocol reasons. |
| `Break` | Reject the whole incoming IQ with XEP-0166 `conflict` + `tie-break`. Semantic policy, including whether the local role is allowed to win, belongs to the resolver. |
| `Postpone` | Continue processing the incoming IQ, but remember this resolver until the correlated local IQ finishes. If that local IQ succeeds, the peer accepted our proposal and no tie-break retry is necessary. If it fails, the resolver may reconcile its still-current local intent afterwards. |

All resolvers registered for the matching `Action` are called. This is deliberate: a resolver may
have owner-local side effects such as updating transport-selection hints. The aggregate wire
result has deterministic precedence `Break > Postpone > Continue`; a `Break` prevents postponed
retry state from being armed for that incoming IQ.

`Postpone` is bound to an explicit outgoing transaction id, not to an `Application` callback or a
transport state. Completion ordering is significant:

```mermaid
sequenceDiagram
    participant Peer
    participant Push as JTPush
    participant TB as Session::TieBreaker
    participant Owner as Resolver owner

    Owner->>TB: outgoingStarted(action, local XML)
    Owner->>Peer: IQ set
    Peer->>Push: simultaneous IQ set, same action
    Push->>TB: resolveIncoming(action, remote XML)
    TB->>Owner: Resolver::resolve(local, remote)
    Owner-->>TB: Postpone
    Push->>Owner: normal Session processing
    Push->>TB: incomingFinished(Applied/Rejected)

    Peer-->>Owner: IQ result/error for local request
    Owner->>TB: outgoingFinished(transaction, error?)
    Note over TB: local transaction stops being collision-active here
    Owner->>Owner: normal IQ ACK/error callback
    Owner->>TB: outgoingCallbacksFinished(transaction)
    alt local IQ succeeded
        Note over TB: discard postponed recovery
    else local IQ failed and remote outcome is known
        TB->>Owner: retry(RetryContext)
    end
```

Clearing collision-active state before owner callbacks prevents a reentrant incoming IQ from being
mistaken for the action that has already completed. Delaying `retry()` until after those callbacks
lets the resolver inspect the owner's updated live state. `RetryContext` supplies the original
local XML, the competing remote XML, the local stanza error and whether normal processing of the
remote action was applied or rejected. It is context, not a command to replay the old stanza:
`retry()` should reconcile current owner intent and may send a different update or do nothing.

`content-modify` is the first consumer. Each participating `Application` registers a resolver for
its own `(creator,name)` content. An initiator-side collision returns `Break`; a responder-side
collision returns `Postpone`, applies the initiator action normally, and only reconciles its local
direction intent if its already outstanding IQ later fails. Unrelated contents using the same
Jingle action return `Continue`.

`transport-replace` deliberately remains on its specialized handler for now. Its crossed-action
path has established semantics beyond accept/reject: losing peer transport proposals can be used
as advisory sibling hints for `TransportSelector::getAlikeTransport()` / local reselection, and
multi-content replacement currently permits partial success for supported siblings. A later
migration must preserve those behaviours while moving only transaction arbitration into
`TieBreaker`; transport semantics must not be moved into the coordinator.

'''
text = replace_once(text, "## Signaling scheduler\n", section + "## Signaling scheduler\n", "TieBreaker architecture section")
architecture.write_text(text)

plan = ROOT / "docs/jingle-calls-implementation-plan.md"
text = plan.read_text()
anchor = "### A1 [P1] Crossed content-modify не сходится к одному negotiated state\n\n"
status = '''### A1 [P1] Crossed content-modify не сходится к одному negotiated state

Статус реализации 2026-09-14: arbitration вынесен в session-owned `Jingle::TieBreaker` с
динамической регистрацией resolver-ов по `Jingle::Action`. Контракт resolver-а:
`Continue`, `Break`, `Postpone`; все resolver-ы совпавшего action вызываются, итоговый приоритет
`Break > Postpone > Continue`. `Postpone` привязан к конкретному outgoing IQ transaction и вызывает
`retry()` только если этот локальный IQ завершился ошибкой; успешный IQ означает, что peer принял
локальное предложение и recovery не нужен. `retry()` запускается после owner ACK/error callbacks
и получает local/remote XML, local stanza error и outcome обработки remote action.

`content-modify` мигрирован первым consumer-ом на Application-level resolver по `(creator,name)`:
initiator возвращает `Break`, responder — `Postpone`; повторная отправка использует актуальный
Application intent, а не старый stanza snapshot. Dispatcher остаётся общей pre-parse точкой.
`transport-replace` пока намеренно НЕ мигрирован: при переносе необходимо сохранить исторические
sibling transport hints (`getAlikeTransport()`/`selectNextTransport(remoteHint)`), partial batch
semantics и reentrancy guards. CI этого нового refactor-а считать закрывающим gate только после
отдельного успешного run на актуальном head.

'''
text = replace_once(text, anchor, status, "A1 status")
plan.write_text(text)

for path in [architecture, plan]:
    ensure_newline(path)
