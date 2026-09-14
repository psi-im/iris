from pathlib import Path

path = Path("target/src/xmpp/xmpp-im/jingle-application.h")
text = path.read_text()
old = "        std::unique_ptr<ContentModifyTieBreakResolver> _contentModifyTieBreakResolver;\n"
new = "        std::unique_ptr<TieBreaker::Resolver>           _contentModifyTieBreakResolver;\n"
if old not in text:
    raise SystemExit("content-modify resolver member anchor not found")
text = text.replace(old, new, 1)
path.write_text(text.rstrip("\n") + "\n")
if not path.read_bytes().endswith(b"\n"):
    raise SystemExit("missing final newline")
