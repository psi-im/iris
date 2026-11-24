/*
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#ifndef JINGLE_OMEMO_H
#define JINGLE_OMEMO_H

#include <iris/iris_export.h>

namespace XMPP {
class OmemoEncryption;

namespace Jingle {
    class Manager;

    /**
     * Install the OMEMO adapters for JET, SXE and DTLS verification.
     * Optional, once per Jingle manager; the adapters are destroyed with omemo.
     */
    IRIS_EXPORT void enableOmemoEncryption(Manager *manager, OmemoEncryption *omemo);
}
}

#endif // JINGLE_OMEMO_H
