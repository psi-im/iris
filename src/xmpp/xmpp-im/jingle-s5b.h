/*
 * jignle-s5b.h - Jingle SOCKS5 transport
 * Copyright (C) 2019  Sergey Ilinykh
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef JINGLE_S5B_H
#define JINGLE_S5B_H

#include "jingle.h"

namespace XMPP {

class Client;

namespace Jingle {
namespace S5B {

extern const QString NS;

class Candidate {
public:
    enum Type {
        None, // non standard, just a default
        Assisted,
        Direct,
        Proxy,
        Tunnel
    };

    Candidate(const QDomElement &el);
    Candidate(const Candidate &other);
    ~Candidate();
    inline bool isValid() const { return d != nullptr; }

private:
    class Private;
    QSharedDataPointer<Private> d;
};

class Manager;
class Transport : public XMPP::Jingle::Transport
{
    Q_OBJECT
public:
    enum Mode {
        Tcp,
        Udp
    };

    inline Transport() {}
    ~Transport();

    void start();
    bool update(const QDomElement &transportEl);
    QDomElement takeUpdate(QDomDocument *doc);
    bool isValid() const;

    QString sid() const;

private:
    friend class Manager;
    static QSharedPointer<XMPP::Jingle::Transport> createOutgoing(Manager *manager, const Jid &to, const QString &transportSid);
    static QSharedPointer<XMPP::Jingle::Transport> createIncoming(Manager *manager, const Jid &from, const QDomElement &transportEl);

    class Private;
    QScopedPointer<Private> d;
};

class Manager : public TransportManager {
    Q_OBJECT
public:
    Manager(XMPP::Jingle::Manager *manager);
    ~Manager();

    QSharedPointer<XMPP::Jingle::Transport> sessionInitiate(const Jid &to); // outgoing. one have to call Transport::start to collect candidates
    QSharedPointer<XMPP::Jingle::Transport> sessionInitiate(const Jid &from, const QDomElement &transportEl); // incoming

    bool hasTrasport(const Jid &jid, const QString &sid) const;

private:
    class Private;
    QScopedPointer<Private> d;
};

} // namespace S5B
} // namespace Jingle
} // namespace XMPP

#endif // JINGLE_S5B_H
