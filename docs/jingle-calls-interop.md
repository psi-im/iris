# Native Jingle calls interoperability status

This document records verified call-stack baselines and interoperability results. It intentionally distinguishes source-level support, synthetic tests, real packet-path tests and calls against external peers.

## Baseline — 2026-09-11

Iris baseline commit: `874a3a6b2a3d29d4ad3be3a1d848fd14dad0e7de` (`docs/jingle-architecture`).

The pull-request CI run `Build and test #267` completed successfully. The regular CI matrix builds Qt 5/QCA 2, Qt 6/QCA 2, Qt 6/QCA 3 and bundled-QCA configurations, plus desktop and Android variants. `IRIS_ENABLE_SRTP` is OFF by default and the standalone `tests/jingle` project is not executed by that workflow, so this green build is a build/install baseline, not an SRTP or call-interoperability result.

The standalone Jingle test project currently defines:

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

### Current transport profile

The existing ICE Jingle transport uses `urn:xmpp:jingle:transports:ice:0`. It has an `Ice176` network engine, DTLS support and an experimental single-component RTP/RTCP-mux packet path. It does not yet implement or advertise the Stable XEP-0176 namespace `urn:xmpp:jingle:transports:ice-udp:1`.

### External peer baseline

Conversations version/commit: **blocked — not pinned yet**.

Android version/device: **blocked — not recorded yet**.

Server and TURN configuration: **blocked — not recorded yet**.

Sanitized JMI/session-initiate/session-accept/transport-info fixtures: **blocked — no authorized test-call capture has been added yet**.

These unknowns must not be replaced with guessed peer behaviour. When a real test peer is available, record exact versions and add sanitized fixtures under `tests/jingle/fixtures/` before claiming Conversations parity.

## Result matrix

No real end-to-end call result is claimed at this baseline.

| Scenario | Peer | Result | Evidence |
| --- | --- | --- | --- |
| Psi ↔ Psi native audio | not integrated yet | blocked | native Psi call controller/media adapter not implemented |
| Psi ↔ Conversations audio | Conversations not pinned | blocked | no captured fixture or real call |
| Audio + video / BUNDLE | no peer | blocked | shared association/router not implemented |
| TURN relay-only | no peer | blocked | no call-level test |

Update this table only with observed results and the exact commits/library versions used for the run.
