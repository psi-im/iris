/*
 * Copyright (C) 2021  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef XMPP_ABSTRACTSTUNDISCO_H
#define XMPP_ABSTRACTSTUNDISCO_H

#include <QDeadlineTimer>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <qca.h>

#include <functional>
#include <memory>

namespace XMPP {

/**
 * Monitors if new STUN services are available, changed or not available anymore.
 */
class AbstractStunDisco : public QObject {
    Q_OBJECT
public:
    enum Transport : std::uint8_t { Tcp, Udp };
    enum Flag : std::uint8_t { Relay = 0x01, Tls = 0x02, Restricted = 0x04 };
    Q_DECLARE_FLAGS(Flags, Flag)

    struct Service {
        using Ptr = std::shared_ptr<Service>;
        QString             name;
        QString             username;
        QCA::SecureArray    password;
        QString             host;
        QList<QHostAddress> addresses4;
        QList<QHostAddress> addresses6;
        std::uint16_t       port = 0;
        Transport           transport;
        Flags               flags;
        QDeadlineTimer      expires;
    };

    using QObject::QObject;

    /**
     * Check where initial discovery is still in progress and therefore it's worth waiting for completion
     */
    virtual bool isDiscoInProgress() const = 0;

Q_SIGNALS:
    void discoFinished(); // if impl did rediscovery, it will signal when finished. required for initial start()
    void serviceAdded(Service::Ptr);
    void serviceRemoved(Service::Ptr);
    void serviceModified(Service::Ptr);
};

} // namespace XMPP

Q_DECLARE_OPERATORS_FOR_FLAGS(XMPP::AbstractStunDisco::Flags)

#endif // XMPP_ABSTRACTSTUNDISCO_H
