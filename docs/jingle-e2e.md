# End-to-end encryption for Jingle sessions

Design notes for an "end-to-end required" policy on Jingle sessions: what the deployed clients
do, what Iris has, and how Iris and Psi should implement it. Nothing here is implemented yet.

## Goal

- A session can be marked as requiring end-to-end encryption (for example a call or a whiteboard
  started from an OMEMO chat, or a file sent from it). This is a local policy, not a wire
  attribute: each side decides what it accepts.
- If no end-to-end protected channel can be established, the session tells the user and sends no
  data: no fallback to an unprotected transport. The peer gets `security-error` (XEP-0166).
- No double encryption: a transport that is end-to-end protected by itself (ICE with DTLS whose
  fingerprint is authenticated) is used as is; a transport without its own protection (S5B, IBB,
  SXE payload in messages) gets a protection layer, or is not used.

DTLS alone is not end-to-end: its fingerprint travels in Jingle signaling through the servers, so
a server can replace it and sit in the middle. It becomes end-to-end once the fingerprint is
authenticated with the peer's OMEMO identity.

## Specifications

| Specification | Status | Relevance |
| --- | --- | --- |
| XEP-0420 Stanza Content Encryption | Experimental | Can encrypt `<iq/>` as well as `<message/>`; no client encrypts Jingle IQs, so it is not used here |
| XEP-0391 Jingle Encrypted Transports (JET) | Deferred (0.1.2, 2018) | `<security/>` in a Jingle content with an encrypted transport key; examples only for file transfer |
| XEP-0396 JET-OMEMO | Deferred (0.2.0, 2018) | The JET envelope is an OMEMO key transport message; examples use the legacy OMEMO namespace |
| XEP-0320 DTLS-SRTP in Jingle | Draft | `<fingerprint xmlns='urn:xmpp:jingle:apps:dtls:0'/>` in the transport |
| OMEMO DTLS verification | Not a XEP | Conversations' namespace, implemented by Conversations and Dino (below) |

Encrypting whole Jingle IQs with SCE would hide signaling from servers, but no client supports it
and it adds no protection for the media: only the DTLS fingerprint needs authentication.

## What Conversations and Dino do

Read from the sources of Conversations (`bf3269cd`, 2026-09-28) and Dino (`8d49d83b`, 2026-08-21).
Both use the legacy OMEMO profile (`eu.siacs.conversations.axolotl`); neither implements OMEMO 2
(`urn:xmpp:omemo:2`) at these revisions.

### OMEMO-verified DTLS fingerprints (calls)

Namespace: `http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification` (sic, "dlts"). Classes:
Conversations `AxolotlService.encrypt(IceUdpTransportInfo, …)`/`decrypt(…)`,
`OmemoVerifiedIceUdpTransportInfo`, `JingleRtpConnection`; Dino
`plugins/omemo/src/dtls_srtp_verification_draft.vala`.

- In every `<transport xmlns='urn:xmpp:jingle:transports:ice-udp:1'>` the plain
  `<fingerprint xmlns='urn:xmpp:jingle:apps:dtls:0' hash='…' setup='…'>HEX</fingerprint>` is
  replaced by

  ```xml
  <fingerprint xmlns='http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification'
               hash='sha-256' setup='actpass'>
    <encrypted xmlns='eu.siacs.conversations.axolotl'>
      <header sid='OWN-DEVICE-ID'>
        <key rid='PEER-DEVICE-ID'>…</key>
        <iv>…</iv>
      </header>
      <payload>…</payload>   <!-- the fingerprint text -->
    </encrypted>
  </fingerprint>
  ```

  `hash` and `setup` stay in clear; the fingerprint value is the OMEMO payload, encrypted for one
  device of the peer.
- The device is agreed through XEP-0353: the responder adds
  `<device xmlns='http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification' id='OWN-DEVICE-ID'/>`
  to `<proceed/>`, and the initiator encrypts for that device. The responder answers for the
  device in the received OMEMO header (`sid`). Dino also puts the `<device/>` in MUJI presence.
  Without a proposal (direct `session-initiate`) there is no device id and no verification.
- `content-add` and `content-accept` carry encrypted fingerprints too; Dino expects them.
- Only `ice-udp:1` transports are handled by both clients.
- Conversations falls back to a plain fingerprint when encryption fails, unless
  `Config.REQUIRE_RTP_VERIFICATION` is set; then the call fails. Dino marks the content as
  OMEMO-verified when decryption succeeds.

### JET-OMEMO (file transfer)

Disco features `urn:xmpp:jingle:jet:0` and `urn:xmpp:jingle:jet-omemo:0`. Classes:
Conversations `Content.setSecurity()/getSecurity()`, `JingleFileTransferConnection`; Dino
`plugins/omemo/src/jingle/jet_omemo.vala`, `xmpp-vala/src/module/xep/0391_jingle_encrypted_transports.vala`.

```xml
<content creator='initiator' name='…'>
  <description xmlns='urn:xmpp:jingle:apps:file-transfer:5'>…</description>
  <transport xmlns='urn:xmpp:jingle:transports:s5b:1' …/>
  <security xmlns='urn:xmpp:jingle:jet:0' name='CONTENT-NAME'
            cipher='urn:xmpp:ciphers:aes-128-gcm-nopadding'
            type='eu.siacs.conversations.axolotl'>
    <encrypted xmlns='eu.siacs.conversations.axolotl'>
      <header sid='…'><key rid='…'>…</key>…<iv>…</iv></header>   <!-- no payload -->
    </encrypted>
  </security>
</content>
```

- The envelope is an OMEMO key transport message: the encrypted key is the 16-byte AES key, the
  header `<iv/>` the 12-byte IV, for all devices of the recipient.
- The whole file is one AES-128-GCM stream with the 16-byte tag appended at the end
  (Conversations: BouncyCastle `GCMBlockCipher` streams; Dino: `SymmetricCipher("AES-GCM")`
  converter streams). The cipher attribute is written without the `:0` suffix.
- The plaintext is authenticated only at the end, so a receiver must keep the data as unverified
  until the tag matches. Ranged transfers and resumption are not covered.

## Iris today

- `OmemoEncryption` (`xmpp_omemo.h`) implements OMEMO 2 and the legacy profile, but as an
  `EncryptionMethod` it only offers `XmppStanza`. Internally the legacy path already supports a
  message without payload (key transport) and per-device key encryption.
- Missing public API: encrypt a payload or a key for a bare JID and a set of devices (one device
  for DTLS verification, all for JET) and return the `<encrypted/>` element; decrypt such an
  element and return the plaintext or key, the sender device and its trust level.
- No JET, no DTLS fingerprint verification, no session-level security policy.

## Plan

1. **OMEMO primitives.** Public API on `OmemoEncryption` for single-element encryption and
   decryption in both profiles, with trust checks: an end-to-end session requires a trusted device
   under the user's trust settings. The profile is chosen per peer from its capabilities
   (`OmemoEncryption::protocolsFor()`): OMEMO 2 when the peer advertises it **and** a feature
   saying it handles OMEMO 2 inside Jingle (for now a Psi feature, later one of a XEP), since
   OMEMO 2 support for messages does not imply it for JET or DTLS verification; otherwise the
   legacy profile, which Conversations and Dino understand; otherwise no end-to-end protection.
   For JET the chosen profile is visible in the `type` attribute of `<security/>`, which XEP-0396
   already ties to the OMEMO namespace.
2. **Session policy.** `Jingle::Session` gets an "end-to-end required" flag. Each transport
   reports its protection: none, encrypted but unauthenticated (DTLS with a plain fingerprint),
   end-to-end. Transport selection and fallback skip transports that cannot reach end-to-end
   under the policy; a content that ends up without protection is rejected or removed with
   `security-error`, and the application shows why.
3. **OMEMO-verified DTLS.** Compatible with Conversations and Dino: the device id in XEP-0353
   `<proceed/>`, encrypted fingerprints in `session-initiate`, `session-accept`, `content-add`,
   `content-accept` and `transport-replace` of ICE transports. Iris also applies it to `ice:0`
   transports and to SCTP data channels (file transfer, whiteboards), which share the DTLS
   association; Conversations and Dino only look at `ice-udp:1`.
4. **JET-OMEMO for S5B and IBB.** Compatible format and whole-stream AES-128-GCM, with the
   received file kept unverified until the tag matches.
5. **SXE payload in messages.** Under the policy the message transport is not used until its
   `<sxe/>` payload is OMEMO-encrypted (SCE); a whiteboard then needs a connection.

## Proposals

- A XEP for OMEMO-verified DTLS fingerprints (the de facto protocol above), with OMEMO 2 and a
  defined behaviour for `ice:0`, `content-add` and transport replacement.
- JET with authenticated chunks (AEAD per chunk, nonce derived from the IV and the chunk number,
  a final-chunk marker), so data can be verified while it streams and ranges can be resumed.
