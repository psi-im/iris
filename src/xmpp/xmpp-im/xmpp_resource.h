/*
 * Copyright (C) 2003  Justin Karneges
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef XMPP_RESOURCE_H
#define XMPP_RESOURCE_H

#include "xmpp_status.h"

#include <QString>

namespace XMPP {
    class Resource
    {
    public:
        Resource(const QString &name="", const Status &s=Status());

        const QString & name() const;
        int priority() const;
        const Status & status() const;

        void setName(const QString &);
        void setStatus(const Status &);

    private:
        QString v_name;
        Status v_status;
    };
} // namespace XMPP

#endif // XMPP_RESOURCE_H
