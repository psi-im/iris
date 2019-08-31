/*
 * Copyright (C) 2006  Remko Troncon
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

#ifndef XMPP_ADDRESS_H
#define XMPP_ADDRESS_H

#include "xmpp/jid/jid.h"

#include <QString>

class QDomElement;

namespace XMPP {
    class Address
    {
    public:
        typedef enum { Unknown, To, Cc, Bcc, ReplyTo, ReplyRoom, NoReply, OriginalFrom, OriginalTo } Type;

        Address(Type type = Unknown, const Jid& jid = Jid());
        Address(const QDomElement&);

        const Jid& jid() const;
        const QString& uri() const;
        const QString& node() const;
        const QString& desc() const;
        bool delivered() const;
        Type type() const;

        QDomElement toXml(Stanza&) const;
        void fromXml(const QDomElement& t);

        void setJid(const Jid &);
        void setUri(const QString &);
        void setNode(const QString &);
        void setDesc(const QString &);
        void setDelivered(bool);
        void setType(Type);

    private:
        Jid v_jid;
        QString v_uri, v_node, v_desc;
        bool v_delivered;
        Type v_type;
    };

    typedef QList<Address> AddressList;
} // namespace XMPP

#endif // XMPP_ADDRESS_H
