from pathlib import Path

p = Path('docs/jingle.md')
s = p.read_text()
marker = '''This allows RTP and an ICE/SCTP file
transfer to share one ICE/DTLS path without making IBB or S5B implicitly shareable.
'''
insert = marker + '''
### Extending BUNDLE in an active session

Initial BUNDLE negotiation and an active-session `content-add` use different validation rules.
`Session::negotiatedGroupings()` is the committed topology used by transport replacement and
runtime association ownership. `groupings()` and `remoteGroupings()` may temporarily describe a
local or peer proposal and must not be treated as committed while an active extension is pending.

An active grouping update may preserve the committed topology or append exactly one content from
the current stanza to exactly one established `BUNDLE` group. Existing members and their order
are immutable for this transaction: removal, reordering, moving an established content between
groups, ambiguous names, and simultaneous edits to multiple groups are rejected. Active grouping
references are resolved against the union of established session contents and contents in the
current stanza rather than against the stanza alone.

For a compatible ICE content, `ICE::Pad` stages a provisional membership on the established
physical `IceConnection`; preparation may use that connection, but the membership is not yet
published in the committed Session topology. The initiator commits only after a matching
`content-accept`. The responder commits only after its `content-accept` IQ is acknowledged. A
local rejection, peer rejection/IQ error, content teardown, or failed commit rolls back only the
new provisional membership, leaving existing RTP members and the shared ICE/DTLS association
alive. A responder may also accept the new content independently by answering with the previous
committed grouping rather than the proposed extension.
'''
if marker not in s:
    raise SystemExit('Jingle shared-transport documentation marker not found')
p.write_text(s.replace(marker, insert, 1))
