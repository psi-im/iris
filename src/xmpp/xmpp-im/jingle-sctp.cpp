/*
 * jignle-sctp.cpp - Jingle SCTP
 * Copyright (C) 2021  Sergey Ilinykh
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

#include "jingle-sctp.h"

#include "jingle-sctp-association_p.h"
#include "jingle-transport.h"
#include "jingle-webrtc-datachannel_p.h"
#include "xmpp_xmlcommon.h"

#include <QQueue>
#include <QtEndian>

#include <mutex>

#define SCTP_DEBUG(msg, ...) qDebug("jingle-sctp: " msg, ##__VA_ARGS__)

namespace XMPP { namespace Jingle { namespace SCTP {

    QDomElement MapElement::toXml(QDomDocument *doc) const
    {
        QDomElement ret;
        if (protocol == Protocol::None)
            return ret;
        ret = doc->createElementNS(ns(), QLatin1String("sctpmap"));
        ret.setAttribute(QLatin1String("protocol"), QLatin1String("webrtc-datachannel"));
        ret.setAttribute(QLatin1String("number"), port);
        return ret;
    }

    bool MapElement::parse(const QDomElement &el)
    {
        if (el.namespaceURI() != ns()) {
            return false;
        }
        auto p   = el.attribute(QLatin1String("protocol"));
        protocol = (p == QLatin1String("webrtc-datachannel")) ? Protocol::WebRTCDataChannel : Protocol::None;
        port     = el.attribute(QLatin1String("number")).toInt();
        return protocol != Protocol::None && port > 0;
    }

    QDomElement ChannelElement::toXml(QDomDocument *doc) const
    {
        auto el = doc->createElementNS(ns(), QLatin1String("channel"));
        el.setAttribute(QLatin1String("id"), id);
        el.setAttribute(QLatin1String("maxPacketLifeTime"), maxPacketLifeTime);
        el.setAttribute(QLatin1String("maxRetransmits"), maxRetransmits);
        el.setAttribute(QLatin1String("negotiated"), negotiated);
        el.setAttribute(QLatin1String("ordered"), ordered);
        el.setAttribute(QLatin1String("protocol"), protocol);
        return el;
    }

    bool ChannelElement::parse(const QDomElement &el)
    {
        if (el.namespaceURI() != webrtcDcNs()) {
            return false;
        }
        bool ok;
        id = el.attribute(QLatin1String("id")).toUShort(&ok); // REVIEW XEP says id is optional. but is it?
        if (!ok)
            return false;
        QString mplt = el.attribute(QLatin1String("maxPacketLifeTime"));
        QString mrtx = el.attribute(QLatin1String("maxRetransmits"));
        if (!mplt.isEmpty()) {
            maxPacketLifeTime = mplt.toUShort(&ok);
            if (!ok)
                return false;
        }
        if (!mrtx.isEmpty()) {
            maxRetransmits = mrtx.toUShort(&ok);
            if (!ok)
                return false;
        }
        if (maxPacketLifeTime > 0 && maxRetransmits > 0) {
            qWarning("found both maxPacketLifeTime and maxRetransmits. expected just one of them");
            return false;
        }
        XMLHelper::readBoolAttribute(el, QLatin1String("negotiated"), &negotiated);
        XMLHelper::readBoolAttribute(el, QLatin1String("ordered"), &ordered);
        protocol = el.attribute(QLatin1String("protocol"));
        return true;
    }

    QString ns() { return QStringLiteral("urn:xmpp:jingle:transports:dtls-sctp:1"); }
    QString webrtcDcNs() { return QStringLiteral("urn:xmpp:jingle:transports:webrtc-datachannel:0"); }

    Association::Association(QObject *parent) : QObject(parent), d(new AssociationPrivate(this)) { }

    void Association::setIdSelector(Association::IdSelector selector)
    {
        switch (selector) {
        case IdSelector::Even:
            d->useOddStreamId = false;
            if (d->nextStreamId & 1)
                d->nextStreamId++;
            break;
        case IdSelector::Odd:
            d->useOddStreamId = true;
            if (!(d->nextStreamId & 1))
                d->nextStreamId++;
            break;
        }
    }

    QByteArray Association::readOutgoing()
    {
        SCTP_DEBUG("read outgoing");
        std::lock_guard<std::mutex> lock(d->mutex);
        return d->outgoingQueue.isEmpty() ? QByteArray() : d->outgoingQueue.dequeue();
    }

    void Association::writeIncoming(const QByteArray &data)
    {
        SCTP_DEBUG("write incoming");
        d->assoc.ProcessSctpData(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    }

    int Association::pendingOutgoingDatagrams() const { return d->outgoingQueue.size(); }

    int Association::pendingChannels() const { return d->pendingChannels.size(); }

    Connection::Ptr Association::nextChannel()
    {
        if (d->pendingChannels.empty())
            return {};
        return d->pendingChannels.dequeue();
    }

    Connection::Ptr Association::newChannel(Reliability reliable, bool ordered, quint32 reliability, quint16 priority,
                                            const QString &label, const QString &protocol)
    {
        int channelType = int(reliable);
        if (ordered)
            channelType |= 0x80;
        auto channel
            = QSharedPointer<WebRTCDataChannel>::create(d.data(), channelType, priority, reliability, label, protocol);
        if (d->transportConnected) {
            auto id = d->takeNextStreamId();
            if (id == 0xffff)
                return {};
            channel->setStreamId(id);
            d->channels.insert(id, channel);
            d->channelsLeft--;
            qWarning("TODO negotiate datachannel itself");
        } else {
            d->pendingLocalChannels.enqueue(channel);
        }

        return channel;
    }

    QList<Connection::Ptr> Association::channels() const
    {
        QList<Connection::Ptr> ret;
        ret.reserve(d->channels.size() + d->pendingLocalChannels.size());
        ret += d->channels.values();
        ret += d->pendingLocalChannels;
        return ret;
    }

    void Association::onTransportConnected()
    {
        SCTP_DEBUG("starting sctp association");
        d->transportConnected = true;
        while (d->pendingLocalChannels.size()) {
            auto channel = d->pendingLocalChannels.dequeue().staticCast<WebRTCDataChannel>();
            auto id      = d->takeNextStreamId();
            if (id == 0xffff) { // impossible channel
                channel->onError(QAbstractSocket::SocketResourceError);
            } else {
                channel->setStreamId(id);
                d->channels.insert(id, channel);
                d->channelsLeft--;
            }
        }
        d->assoc.TransportConnected();
    }

    void Association::onTransportError(QAbstractSocket::SocketError error)
    {
        d->transportConnected = false;
        for (auto &c : d->channels) {
            c.staticCast<WebRTCDataChannel>()->onError(error);
        }
    }

    void Association::onTransportClosed()
    {
        d->transportConnected = false;
        for (auto &c : d->channels) {
            c.staticCast<WebRTCDataChannel>()->onDisconnected(WebRTCDataChannel::TransportClosed);
        }
    }

}}}
