# Native Jingle calls interoperability status

This document records verified call-stack baselines and interoperability results. It intentionally distinguishes source-level support, synthetic tests, real packet-path tests and calls against external peers.

## Current audit snapshot — 2026-09-13

Reviewed ranges:

- Psi `9421bd0e` → `c8821dbe`.
- Iris `ba784334` → `be3833e`.
- psimedia `ab15f68` → `b4139cbd`.

Local Iris build used one `-j2` job, Qt 6.10.2 / QCA3 3.0.3 with SRTP and SCTP.
Serial CTest with loopback socket permission: **24/24 passed**. This includes the new
content-modify, content-modify-race and RTP direction tests. The standalone Psi
`src/avcall/unittest/policy.cpp` was compiled and passed separately; this is not a full Psi build.

An extra temporary reproducer using the actual Iris Application/ACK code and Psi policy
helper confirms two uncovered scenarios: crossed requests leave divergent senders, and a
failed request is discarded by Iris while the client policy still suppresses the same target.
This is application/helper-level evidence plus dispatcher source review, not a live two-peer
XMPP run. Permanent two-Session/controller regressions remain required.

psimedia runtime tests were not run locally: `gstreamer-app-1.0` development pkg-config
dependency is missing. New remote CI runs, sanitizer runs and live calls were not verified
in this audit. Source review confirms production bridge wiring and live-ID rebuild, but also
the exclusion of live/file transitions and the reset of receive/video alongside mic changes.
The audio-only sender test is not evidence of physical capture release or A/V continuity.

See [the development plan](jingle-calls-implementation-plan.md#2-новый-audit-gate-исправления-доказательства-и-оставшиеся-проблемы)
for corrective actions and required regression scenarios, and
[native RTP architecture](jingle-rtp-design.md#psi-and-psimedia-production-boundary)
for the current production packet path. No Conversations parity or live BUNDLE is claimed.

## Earlier Iris snapshot — 36f9e3a

Reviewed changes from `7c0acb0c7dc45a9173faba5521f551916e9d67d7` to
`36f9e3ac5e326633b56cd24c2d2c8dea4b638732`: guarded initial-subset cleanup,
empty initial-answer rejection and runtime SSRC retention across router reconfiguration.
Psi and psimedia were not re-audited for this snapshot.

Local verification: Qt 6.10.2, system QCA3 3.0.3, SRTP and SCTP enabled;
one build with `-j2`, serial CTest: **21/21 passed**. Loopback tests require socket
permissions; the sandbox-only attempt could not bind UDP, while the permitted run passed.
This includes both `ice:0` and `ice-udp:1` local packet paths, not external peers.

The working-tree acceptance regressions additionally cover removal before queued start,
self/sibling deletion from start callbacks, empty-session termination, Session cancellation
and destruction, and later content acceptance without duplicate activation.
With this working-tree fix, the full serial Jingle suite passes **21/21** in the same environment.
See [scheduling semantics](jingle.md#session-state-versus-connectivity).

Standard ICE-UDP, native RTP and asynchronous media interfaces are implemented.
Group/membership and BundleRouter components have standalone tests, but are not wired
into a production shared association. No live BUNDLE or Conversations result is claimed.
See [native RTP architecture](jingle-rtp-design.md) for the current layer boundaries.

The dated baseline below is retained as a record of that run, not a description of
current source capabilities. Its absent Psi adapter is not a current Iris finding.

## Historical baseline — 2026-09-11

Iris baseline commit: `874a3a6b2a3d29d4ad3be3a1d848fd14dad0e7de` (`docs/jingle-architecture`).

The pull-request CI run `Build and test #267` completed successfully. The regular CI matrix builds Qt 5/QCA 2, Qt 6/QCA 2, Qt 6/QCA 3 and bundled-QCA configurations, plus desktop and Android variants. `IRIS_ENABLE_SRTP` is OFF by default and the standalone `tests/jingle` project is not executed by that workflow, so this green build is a build/install baseline, not an SRTP or call-interoperability result.

At this baseline, the standalone Jingle test project defines:

- `jingle_signaling`
- `jingle_dtlssrtp`
- `jingle_iceownership`
- `jingle_rtpdescription`
- `jingle_rtpnegotiation`
- `jingle_rtpapplication`
- `jingle_srtp`
- `jingle_transportacks`
- `jingle_icertp` and `jingle_rtpmedia` when QCA 3 and SRTP are enabled

The last two targets exercise real local UDP/ICE/DTLS/SRTP with mock application/media integration. They are not evidence of a call through an XMPP server or interoperability with Conversations.

### Baseline transport profile

The existing ICE Jingle transport uses `urn:xmpp:jingle:transports:ice:0`. It has an `Ice176` network engine, DTLS support and an experimental single-component RTP/RTCP-mux packet path. It does not yet implement or advertise the Stable XEP-0176 namespace `urn:xmpp:jingle:transports:ice-udp:1`.

### External peer baseline

Conversations version/commit: **blocked — not pinned yet**.

Android version/device: **blocked — not recorded yet**.

Server and TURN configuration: **blocked — not recorded yet**.

Sanitized JMI/session-initiate/session-accept/transport-info fixtures: **blocked — no authorized test-call capture has been added yet**.

These unknowns must not be replaced with guessed peer behaviour. When a real test peer is available, record exact versions and add sanitized fixtures under `tests/jingle/fixtures/` before claiming Conversations parity.

### Baseline result matrix

No real end-to-end call result is claimed at this baseline.

| Scenario | Peer | Result | Evidence |
| --- | --- | --- | --- |
| Psi ↔ Psi native audio | not integrated yet | blocked | native Psi call controller/media adapter not implemented |
| Psi ↔ Conversations audio | Conversations not pinned | blocked | no captured fixture or real call |
| Audio + video / BUNDLE | no peer | blocked | shared association/router not implemented |
| TURN relay-only | no peer | blocked | no call-level test |

Update this table only with observed results and the exact commits/library versions used for the run.
