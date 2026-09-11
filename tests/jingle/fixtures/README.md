# Jingle interoperability fixtures

This directory is reserved for sanitized protocol fixtures captured from authorized test sessions.

Each fixture added here must document:

- peer product and exact version/commit when known;
- Iris/Psi commit used for the capture;
- Android/desktop platform where relevant;
- server and TURN configuration relevant to the protocol flow;
- whether the XML is an exact capture or a minimized reproduction;
- which secrets or identifying values were replaced.

Do not commit real ICE/TURN passwords, DTLS/SRTP key material, private keys, account JIDs, public IP addresses or unrelated message content. Use documentation/example address ranges and obvious placeholder credentials when normalization is required.

Keep synthetic fixtures clearly labelled as synthetic. A synthetic XEP example is useful for parser tests but is not evidence of Conversations interoperability.

## Current status

As of 2026-09-11 no Conversations fixture has been committed. The peer version and a sanitized authorized test-call capture are still external prerequisites for the interoperability matrix in `docs/jingle-calls-interop.md`.
