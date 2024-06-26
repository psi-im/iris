/*
 * jignle-connection.h - Jingle Connection - minimal data transfer unit for an application
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

#ifndef JINGLE_CONNECTION_H
#define JINGLE_CONNECTION_H

/**
 * A transport may have multiple connections.
 * For example an ICE transport may have up to 65537 connections (65535 data/sctp-channels + 2 raw)
 */

#include "iris/bytestream.h"
#include "jingle.h"

#include <QNetworkDatagram>

namespace XMPP { namespace Jingle {

    class Connection : public ByteStream {
        Q_OBJECT
    public:
        using Ptr      = QSharedPointer<Connection>; // will be shared between transport and application
        using ReadHook = std::function<void(char *, qint64)>;

        virtual bool              hasPendingDatagrams() const;
        virtual QNetworkDatagram  readDatagram(qint64 maxSize = -1);
        virtual bool              writeDatagram(const QNetworkDatagram &data);
        virtual size_t            blockSize() const;
        virtual int               component() const;
        virtual TransportFeatures features() const = 0;

        inline void setId(const QString &id) { _id = id; }
        inline bool isRemote() const { return _isRemote; }
        inline void setRemote(bool value) { _isRemote = value; }
        inline void setReadHook(ReadHook hook) { _readHook = hook; }

    signals:
        void connected();
        void disconnected();

    protected:
        qint64 writeData(const char *data, qint64 maxSize);
        qint64 readData(char *buf, qint64 maxSize) final;

        // same rules as for QIOdevice::readData. It was just necessary to wrap it.
        virtual qint64 readDataInternal(char *data, qint64 maxSize) = 0;

        bool     _isRemote = false;
        QString  _id;
        ReadHook _readHook;
    };

    using ConnectionAcceptorCallback = std::function<bool(Connection::Ptr)>;
    struct ConnectionAcceptor {
        TransportFeatures          features;
        ConnectionAcceptorCallback callback;
        int                        componentIndex;
    };
}}

#endif // JINGLE_CONNECTION_H
