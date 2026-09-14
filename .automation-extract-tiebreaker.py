from pathlib import Path


def ensure_newline(path: Path) -> None:
    data = path.read_text()
    path.write_text(data.rstrip("\n") + "\n")


root = Path("target")

jingle_h = root / "src/xmpp/xmpp-im/jingle.h"
text = jingle_h.read_text()
start_marker = "    /**\n     * Session-scoped coordinator for simultaneous Jingle actions.\n"
end_marker = "    /*\n    Categorization by speed, reliability and connectivity\n"
start = text.index(start_marker)
end = text.index(end_marker, start)
block = text[start:end].rstrip() + "\n"
text = text[:start] + text[end:]
text = text.replace("#include <functional>\n#include <memory>\n", "#include <functional>\n")
jingle_h.write_text(text)

header = root / "src/xmpp/xmpp-im/jingle-tiebreaker.h"
header.write_text(
    "/*\n"
    " * jingle-tiebreaker.h - Session-scoped Jingle tie-break coordination\n"
    " * Copyright (C) 2026  Sergei Ilinykh\n"
    " *\n"
    " * This library is free software; you can redistribute it and/or\n"
    " * modify it under the terms of the GNU Lesser General Public\n"
    " * License as published by the Free Software Foundation; either\n"
    " * version 2.1 of the License, or (at your option) any later version.\n"
    " */\n\n"
    "#ifndef JINGLE_TIEBREAKER_H\n"
    "#define JINGLE_TIEBREAKER_H\n\n"
    "#include <iris/xmpp-im/jingle.h>\n\n"
    "#include <memory>\n\n"
    "namespace XMPP { namespace Jingle {\n\n"
    + block
    + "\n}}\n\n"
    "#endif // JINGLE_TIEBREAKER_H\n"
)

jingle_cpp = root / "src/xmpp/xmpp-im/jingle.cpp"
text = jingle_cpp.read_text()
impl_start_marker = "    struct TieBreaker::SharedState {\n"
impl_end_marker = "    class Jingle::Private : public QSharedData {\n"
impl_start = text.index(impl_start_marker)
impl_end = text.index(impl_end_marker, impl_start)
impl = text[impl_start:impl_end].rstrip() + "\n"
text = text[:impl_start] + text[impl_end:]
jingle_cpp.write_text(text)

source = root / "src/xmpp/xmpp-im/jingle-tiebreaker.cpp"
source.write_text(
    "/*\n"
    " * jingle-tiebreaker.cpp - Session-scoped Jingle tie-break coordination\n"
    " * Copyright (C) 2026  Sergei Ilinykh\n"
    " *\n"
    " * This library is free software; you can redistribute it and/or\n"
    " * modify it under the terms of the GNU Lesser General Public\n"
    " * License as published by the Free Software Foundation; either\n"
    " * version 2.1 of the License, or (at your option) any later version.\n"
    " */\n\n"
    "#include \"jingle-tiebreaker.h\"\n\n"
    "#include <QDomElement>\n"
    "#include <QHash>\n\n"
    "namespace XMPP { namespace Jingle {\n\n"
    + impl
    + "\n}}\n"
)

session_h = root / "src/xmpp/xmpp-im/jingle-session.h"
text = session_h.read_text()
anchor = "#include <iris/xmpp-im/jingle-application.h>\n"
if "#include <iris/xmpp-im/jingle-tiebreaker.h>\n" not in text:
    text = text.replace(anchor, anchor + "#include <iris/xmpp-im/jingle-tiebreaker.h>\n", 1)
session_h.write_text(text)

cmake = root / "src/xmpp/CMakeLists.txt"
text = cmake.read_text()
text = text.replace(
    "    xmpp-im/jingle-session.h\n",
    "    xmpp-im/jingle-session.h\n    xmpp-im/jingle-tiebreaker.h\n",
    1,
)
text = text.replace(
    "    xmpp-im/jingle.cpp\n    xmpp-im/jingle-connection.cpp\n",
    "    xmpp-im/jingle.cpp\n    xmpp-im/jingle-tiebreaker.cpp\n    xmpp-im/jingle-connection.cpp\n",
    1,
)
cmake.write_text(text)

forwarder = root / "include/iris/jingle-tiebreaker.h"
forwarder.write_text("#include <iris/xmpp-im/jingle-tiebreaker.h>\n")

for path in [jingle_h, header, jingle_cpp, source, session_h, cmake, forwarder]:
    ensure_newline(path)

# Refuse to leave any touched text file without a final newline.
for path in [jingle_h, header, jingle_cpp, source, session_h, cmake, forwarder]:
    if not path.read_bytes().endswith(b"\n"):
        raise SystemExit(f"missing final newline: {path}")
