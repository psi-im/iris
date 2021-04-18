/*
 * jignle-sctp-association_p.h - Private parto of Jingle SCTP Association
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

#include "jingle-sctp-association_p.h"
#include "jingle-sctp.h"
#include "jingle-webrtc-datachannel_p.h"

namespace XMPP { namespace Jingle { namespace SCTP {

    static constexpr int MAX_STREAMS          = 65535; // let's change when we need something but webrtc dc.
    static constexpr int MAX_MESSAGE_SIZE     = 262144;
    static constexpr int MAX_SEND_BUFFER_SIZE = 262144;

    std::weak_ptr<Keeper> Keeper::instance;

    Keeper::Keeper() { DepUsrSCTP::ClassInit(); }

    Keeper::~Keeper() { DepUsrSCTP::ClassDestroy(); }

    Keeper::Ptr Keeper::use()
    {
        auto i = instance.lock();
        if (!i) {
            i        = std::make_shared<Keeper>();
            instance = i;
        }
        return i;
    }

    AssociationPrivate::AssociationPrivate(Association *q) :
        q(q), keeper(Keeper::use()), assoc(this, MAX_STREAMS, MAX_STREAMS, MAX_MESSAGE_SIZE, MAX_SEND_BUFFER_SIZE, true)
    {
    }

    void AssociationPrivate::OnSctpAssociationConnecting(RTC::SctpAssociation *)
    {
        qDebug("jignle-sctp: on connecting");
    }

    void AssociationPrivate::OnSctpAssociationConnected(RTC::SctpAssociation *) { qDebug("jignle-sctp: on connected"); }

    void AssociationPrivate::OnSctpAssociationFailed(RTC::SctpAssociation *) { qDebug("jignle-sctp: on failed"); }

    void AssociationPrivate::OnSctpAssociationClosed(RTC::SctpAssociation *) { qDebug("jignle-sctp: on closed"); }

    void AssociationPrivate::OnSctpAssociationSendData(RTC::SctpAssociation *, const uint8_t *data, size_t len)
    {
        qDebug("jignle-sctp: on outgoing data");
        QByteArray bytes((char *)data, len);
        QMetaObject::invokeMethod(this, "onOutgoingData", Q_ARG(QByteArray, bytes));
    }

    void AssociationPrivate::OnSctpAssociationMessageReceived(RTC::SctpAssociation *, uint16_t streamId, uint32_t ppid,
                                                              const uint8_t *msg, size_t len)
    {
        qDebug("jignle-sctp: on incoming data");
        QByteArray bytes((char *)msg, len);
        QMetaObject::invokeMethod(this, "onIncomingData", Q_ARG(QByteArray, bytes), Q_ARG(quint16, streamId),
                                  Q_ARG(quint32, ppid));
    }

    void AssociationPrivate::OnSctpAssociationBufferedAmount(RTC::SctpAssociation *sctpAssociation, uint32_t len)
    {
        qDebug("jignle-sctp: on buffered data: %d", len);
        Q_UNUSED(sctpAssociation);
        Q_UNUSED(len);
        // TODO control buffering to reduce memory consumption
    }

    void AssociationPrivate::OnSctpStreamClosed(RTC::SctpAssociation *sctpAssociation, uint16_t streamId)
    {
        qDebug("jignle-sctp: on stream closed");
        Q_UNUSED(sctpAssociation);
        QMetaObject::invokeMethod(this, "onStreamClosed", Q_ARG(quint16, streamId));
    }

    void AssociationPrivate::handleIncomingDataChannelOpen(const QByteArray &data, quint16 streamId)
    {
        auto channel = WebRTCDataChannel::fromChannelOpen(this, data);

        channel->setStreamId(streamId);
        pendingChannels.append(channel);
        channels.insert(streamId, channel);

        // acknowledge channel open instantly
        QByteArray reply(4, 0);
        reply[0] = DCEP_DATA_CHANNEL_ACK;
        write(reply, streamId, PPID_DCEP);

        emit q->newChannel();
    }

    bool AssociationPrivate::write(const QByteArray &data, quint16 streamId, quint32 ppid)
    {
        qDebug("jignle-sctp: write");
        RTC::DataConsumer consumer;
        consumer.sctpParameters.streamId = streamId;
        consumer.sctpParameters.ordered  = true;
        bool success;
        assoc.SendSctpMessage(
            &consumer, ppid, reinterpret_cast<const uint8_t *>(data.data()), data.size(),
            new std::function<void(bool)>([this, &success](bool cb_success) { success = cb_success; }));
        return success;
    }

    void AssociationPrivate::close(quint16 streamId)
    {
        qDebug("jignle-sctp: close");
        RTC::DataProducer producer;
        producer.sctpParameters.streamId = streamId;
        assoc.DataProducerClosed(&producer);
    }

    quint16 AssociationPrivate::takeNextStreamId()
    {
        if (!channelsLeft)
            return 0xffff; // impossible stream
        auto id = nextStreamId;
        while (channels.contains(id)) {
            id += 2;
            if (id == nextStreamId)
                return 0xffff;
        }
        nextStreamId = id + 2;
        return id;
    }

    void AssociationPrivate::onOutgoingData(const QByteArray &data)
    {
        outgoingQueue.enqueue(data);
        emit q->readyReadOutgoing();
    }

    void AssociationPrivate::onIncomingData(const QByteArray &data, quint16 streamId, quint32 ppid)
    {
        auto it = channels.find(streamId);
        if (it == channels.end()) {
            if (ppid == PPID_DCEP) {
                if (data.isEmpty()) {
                    qWarning("jingle-sctp: dropping invalid dcep");
                } else if (data[0] == DCEP_DATA_CHANNEL_OPEN) {
                    handleIncomingDataChannelOpen(data, streamId);
                }
            } else
                qWarning("jingle-sctp: data from unknown datachannel. ignoring");
            return;
        }
        it->staticCast<WebRTCDataChannel>()->onIncomingData(data, ppid);
    }

    void AssociationPrivate::onStreamClosed(quint16 streamId)
    {
        auto it = channels.find(streamId);
        if (it == channels.end()) {
            qDebug("jingle-sctp: closing not existing stream %d", streamId);
            return;
        }
        it->staticCast<WebRTCDataChannel>()->onDisconnected(WebRTCDataChannel::ChannelClosed);
    }

}}}
