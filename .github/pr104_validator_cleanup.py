from pathlib import Path

header = Path('src/xmpp/xmpp-im/jingle-group-negotiation_p.h')
s = header.read_text()
start = s.index('        struct ActiveExtension {')
end = s.index('        enum class Error {', start)
s = s[:start] + s[end:]
start = s.index('        // Validate a content-add grouping update against the already committed')
end = s.index('        // A negotiated multi-content BUNDLE association is one transport', start)
s = s[:start] + s[end:]
s = s.replace('\n#include <algorithm>\n', '\n')
header.write_text(s)

test = Path('tests/jingle/groupnegotiation.cpp')
s = test.read_text()
start = s.index('    // Active content-add may extend exactly one established BUNDLE with exactly')
end = s.index('    const QList<ContentGroup> twoOffers', start)
s = s[:start] + s[end:]
test.write_text(s)
