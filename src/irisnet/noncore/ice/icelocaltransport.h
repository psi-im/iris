/*
 * Copyright (C) 2009-2010  Barracuda Networks, Inc.
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

#ifndef ICELOCALTRANSPORT_H
#define ICELOCALTRANSPORT_H

#include "iceabstractstundisco.h"
#include "icecandidate.h"
#include "icetransport.h"

#include <QByteArray>
#include <QNetworkInterface>
#include <QObject>
#include <memory>

class QHostAddress;
class QUdpSocket;

namespace QCA {
class SecureArray;
}

namespace XMPP::ICE {
// this class manages a single port on a single interface, including the
//   relationship with an associated STUN/TURN server.  if TURN is used, this
//   class offers two paths (0=direct and 1=relayed), otherwise it offers
//   just one path (0=direct)
class LocalTransport : public Transport, public std::enable_shared_from_this<LocalTransport> {
    Q_OBJECT

public:
    using Ptr = std::shared_ptr<LocalTransport>;

    enum Error { ErrorBind = ErrorCustom, ErrorStun, ErrorTurn };

    LocalTransport(QObject *parent = nullptr);
    ~LocalTransport();

    static Ptr make() { return std::make_shared<LocalTransport>(); }

    // if socket is passed turn ErrorMismatch won't be handled (potentially worthen the connectivity)
    void                             setSocket(QUdpSocket *socket, bool borrowedSocket, const LocalAddress &la);
    QUdpSocket                      *takeBorrowedSocket();
    QNetworkInterface::InterfaceType networkType() const;
    TransportAddress                 localAddress() const;
    const QHostAddress              &externalAddress() const;
    void                             setExternalAddress(const QHostAddress &addr);

    void setClientSoftwareNameAndVersion(const QString &str);
    void setStunDiscoverer(AbstractStunDisco *discoverer);

    void start();

    // reimplemented
    void       stop() override;
    bool       hasPendingDatagrams(int path) const override;
    QByteArray readDatagram(int path, TransportAddress &addr) override;
    void       writeDatagram(int path, const QByteArray &buf, const TransportAddress &addr) override;
    void       addChannelPeer(const TransportAddress &addr) override;
    void       setDebugLevel(DebugLevel level) override;
    void       changeThread(QThread *thread) override;

signals:
    // may be emitted multiple times.
    // if handling internal ErrorMismatch, then local address may change
    //   and server reflexive address may disappear.
    // if start(QUdpSocket*) was used, then ErrorMismatch is not handled,
    //   and this signal will only be emitted to add addresses
    // void addressesChanged();

    void candidateFound(std::shared_ptr<CandidateInfo> candidate);

private:
    class Private;
    friend class Private;
    Private *d;
};
} // namespace XMPP

#endif // ICELOCALTRANSPORT_H
