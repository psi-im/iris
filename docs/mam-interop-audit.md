# Iris MAM interoperability audit

Status: **in progress** (started 2026-10-17). Scope: Iris only. Psi history UI, persistence, and account synchronization belong to a subsequent task.

## Sources and scope

Primary standard: [XEP-0313 MAM 1.1.3](https://xmpp.org/extensions/xep-0313.html). Compare behavior actually used by at least one of:

- [Dino `xmpp-vala/src/module/xep/0313_message_archive_management.vala`](https://github.com/dino/dino/blob/master/xmpp-vala/src/module/xep/0313_message_archive_management.vala)
- [Dino `xmpp-vala/src/module/xep/0313_2_message_archive_management.vala`](https://github.com/dino/dino/blob/master/xmpp-vala/src/module/xep/0313_2_message_archive_management.vala)
- [Dino `libdino/src/service/history_sync.vala`](https://github.com/dino/dino/blob/master/libdino/src/service/history_sync.vala)
- [Kaidan](https://invent.kde.org/network/kaidan), which uses [QXmppMamManager](https://api.kde.org/qxmppmammanager.html).
- [Conversations](https://codeberg.org/iNPUTmice/Conversations) and [monocles chat](https://codeberg.org/monocles/monocles_chat): source-by-source audit outstanding. Do not infer every implementation detail merely from monocles' Conversations ancestry.

"Related to MAM" means core archive queries, paging, metadata, synchronization prerequisites, and the interpretation of message elements *replayed via MAM*. It does not imply reimplementing every message-related XEP inside an MAM module.

## Existing Iris implementation: do not duplicate

| XEP / facility | Located in Iris | Follow-up |
| --- | --- | --- |
| XEP-0313 MAM | `xmpp_mamtask.{h,cpp}`, `xmpp_mammanager.{h,cpp}` | **Present but incorrect**; repair in place, not a second MAM manager |
| XEP-0059 Result Set Management | `xmpp_subsets.{h,cpp}` / `SubsetsClientManager` | Reuse for page cursors/limits |
| XEP-0359 Unique and Stable Stanza IDs | `Message::stanzaId()`, `setStanzaId()`, `originId()`, `setOriginId()` in `xmpp_message.h` / `types.cpp` | Check multiplicity and trusted `by` selection; never treat arbitrary IDs as verified archive IDs |
| XEP-0297 Stanza Forwarding | `xmpp_forwarding.{h,cpp}` | Reuse for parsing forwarded message and XEP-0203 delay |
| XEP-0203 Delayed Delivery | Existing timestamp parsing in forwarding/message processing | Check UTC parsing and MAM timestamp handling |
| XEP-0280 Message Carbons | `xmpp_carbons.cpp`; `Message::forwarded()` | Keep separate from archive-result envelope handling and deduplication |
| XEP-0334 Message Processing Hints | `Message::processingHints()` | Preserve hints when replaying messages |
| XEP-0308 Last Message Correction | `Message::replaceId()` | Replayed correction is not a second user message |
| XEP-0424 Message Retraction | Handling found in `types.cpp` | Verify semantics against archived originals/tombstones |
| XEP-0444 Message Reactions | Handling found in `types.cpp` | Verify replay of reactions and deduplication |
| XEP-0425 Message Moderation | No matching handling found in first source search | Verify exact client variant and required MUC archive semantics |
| XEP-0421 Occupant IDs | No matching namespace found in first source search | Validate MUC identity and archived sender handling |
| XEP-0461 Message Replies | No matching `urn:xmpp:reply:0` found in first source search | Check any alternate parsing implementation before adding |
| XEP-0490 Message Displayed Synchronization | No matching namespace found in first source search | Verify relevance as archived-message marker |
| XEP-0441 MAM Preferences | Not found in first MAM source search | Optional protocol, not required for baseline sync; specification discourages use where it interferes with synchronization |
| XEP-0442 PubSub MAM | Not found in first MAM source search | Include only if verified in scope client; avoid speculative support |

Dino's `xmpp-vala/src/module/xep` contains implementations of XEP-0059, -0203, -0280, -0297, -0313, -0308, -0334, -0359, -0421, -0424, -0425, -0444, -0461 and -0490. The last entries are **candidate protocol dependencies for correct history replay**, not necessarily MAM transport features. Their exact implementation and Iris support need detailed comparison.

## Confirmed defects in existing Iris MAMTask

1. `MAMTask::take` ignores the final IQ `<fin/>` and does not use its `complete` attribute or RSM `<first/>` and `<last/>` cursors. XEP-0313 requires finishing a page on this IQ, not on a guessed number of result stanzas. It can hang on short pages.
2. `getPage` passes `mamMaxMessages` to RSM `setMax` instead of `mamPageSize`; limits are conflated. `messagesFetched >= mamMaxMessages` also fires immediately when `mamMaxMessages == 0`, despite documented unlimited semantics.
3. `getPage` can issue the next page on receiving result number `mamPageSize`, **before** receiving the current query's `<fin/>`. This makes query/result correlation and termination unreliable.
4. `onGo()` unconditionally requests `<metadata/>`. This operation requires `urn:xmpp:mam:2#extended` discovery support; ordinary MAM servers must still work without it.
5. `flip-page`, `after-id`, `before-id`, and `include-groupchat` are sent without checking advertised extended/groupchat-field features. XEP-0313 explicitly gates these.
6. Time values use `QDateTime::toString()` without `Qt::ISODate`; the wire format must be XEP-0082 / UTC ISO-8601. `setTimeSpec(Qt::UTC)` also changes an instant's interpretation rather than converting with `toUTC()`.
7. The input JID is always used as a `with` filter. The IQ itself has no archive `to` address, so querying a MUC room archive is not correctly represented. Empty `with` must be omitted for unfiltered catch-up.
8. `MAMTask(const MAMTask&)` aliases the owned raw `Private*` and both destructors delete it. Since `Task` inherits `QObject`, copying must be disabled.
9. The ID-range `get()` overload fails to initialize `Private::q` (used by `getPage` and `getArchiveMetadata`), and both `get` overloads leave fields from the other mode untouched. Initialization/reset invariants need a single internal path.
10. `archive()` buffers raw `QDomElement` values without explicit result metadata or streaming; evaluate bounds, cancellation, and lifetime before relying on it for unbounded catch-up. No dedicated MAM regression tests were found under the current `tests/` directory.

## Proven baseline from Dino

Dino keeps protocol operations separate from the application's `HistorySync`: it creates an IQ addressed to `mam_server`, correlates message results by `queryid`, reads `<fin/>` with the RSM first/last and completion flag, and checks the claimed archive sender before dispatching the forwarded messages. Dino's `HistorySync` then maintains per-account, per-archive ranges and deduplicates messages by archive ID.

**Iris architecture choice:** Keep query construction, result validation, `<fin/>` pagination and optional streaming callback in the existing `MAMTask` / `MAMManager`. Reuse existing `SubsetsClientManager`, `Forwarding`, `Message`, disco and Task infrastructure. Leave persistent cursors, local DB ranges, storage and UI synchronization to Psi; do not create an Iris-level history database.

## Implementation sequence and acceptance

1. Repair and test existing MAM task lifecycle, copy safety, page size/total limits, correct RSM cursor semantics, timestamp encoding, `<fin/>` completion, IQ error propagation, result filtering and cancellation. Test both forward and reverse paging, partial final pages, empty archive, unlimited mode and unknown query IDs.
2. Support unfiltered own-account catch-up and MUC room archives by separating **archive JID** from optional **with-filter JID**; choose features by disco, not hard-coded assumptions. Regression-test normal-only and extended-capable servers.
3. Add optional extended features only when advertised: archive metadata, ID bounds/single-item retrieval, flipped pages and groupchat field. Keep legacy signatures working where possible and avoid inflating public headers.
4. Audit security and semantic replay: correct origin verification, stable-ID trust and deduplication; use existing forwarded/delay/carbon/message parsers. Add missing, demonstrably used message-XEP parsers outside MAM only when genuinely absent.
5. Cross-check current Conversations, monocles, Dino and Kaidan sources; run Qt5/Qt6, unit and sanitizer CI; document tested interop. No Psi integration until separately requested.

Completion requires passing tests and an audited comparison, **not** merely adding namespace strings or declaring feature support.
