/*
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#include "jingle-omemo.h"

#include "jingle-jet-omemo.h"
#include "jingle-omemo-verification.h"
#include "jingle-sxe-omemo.h"
#include "jingle.h"
#include "xmpp_omemo.h"

namespace XMPP::Jingle {
void enableOmemoEncryption(Manager *manager, OmemoEncryption *omemo)
{
    if (!manager || !omemo || manager->dtlsVerification())
        return;

    // JET must precede DTLS verification, which recognizes JET-protected contents.
    new JetOmemo(manager, omemo, omemo);
    new SXE::SxeOmemo(manager, omemo, omemo);
    new OmemoDtlsVerification(manager, omemo, omemo);
}
}
