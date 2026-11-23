/*
 * jingle-jet-connection.cpp - authenticated Jingle byte-stream wrapper
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "jingle-jet-connection.h"

#include "jingle-jet_p.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace XMPP { namespace Jingle { namespace JET {
    Connection::Connection(XMPP::Jingle::Connection::Ptr raw, const QByteArray &key, const QByteArray &iv, bool sender,
                           quint64 plainSize) :
        raw_(std::move(raw)), plainSize_(plainSize), sender_(sender)
    {
        if (!raw_ || plainSize_ > std::numeric_limits<quint64>::max() - StreamCipher::TagSize)
            return;
        cipher_ = sender_ ? StreamCipher::encryptor(key, iv) : StreamCipher::decryptor(key, iv);
        if (!cipher_)
            return;

        setId(raw_->id());
        setRemote(raw_->isRemote());
        open(QIODevice::ReadWrite | QIODevice::Unbuffered);

        connect(raw_.data(), &QIODevice::readyRead, this, &Connection::pump);
        connect(raw_.data(), &QIODevice::bytesWritten, this, [this](qint64 count) {
            if (failed_ || count <= 0)
                return;
            const auto acknowledged = qMin(quint64(count), plainWritten_ - plainAcknowledged_);
            plainAcknowledged_ += acknowledged;
            if (acknowledged)
                emit bytesWritten(qint64(acknowledged));
        });
        connect(raw_.data(), &XMPP::Jingle::Connection::connected, this, &Connection::connected);
        connect(raw_.data(), &XMPP::Jingle::Connection::disconnected, this, &Connection::disconnected);
        connect(raw_.data(), &ByteStream::connectionClosed, this, [this]() {
            if (!sender_ && !verified_ && !failed_)
                fail(QStringLiteral("JET stream closed before authentication completed"));
            emit connectionClosed();
        });
        connect(raw_.data(), &ByteStream::delayedCloseFinished, this, &Connection::delayedCloseFinished);
        connect(raw_.data(), &ByteStream::error, this, [this](int code) {
            if (!failed_)
                setError(code, raw_->errorText());
        });

        // A zero-byte file still has an authentication tag on the wire.
        if (sender_ && !plainSize_)
            finishSending();
        if (!sender_)
            pump();
    }

    Connection::~Connection() = default;

    TransportFeatures Connection::features() const { return raw_ ? raw_->features() : TransportFeatures(); }
    size_t Connection::blockSize() const { return raw_ ? raw_->blockSize() : 0; }
    int Connection::component() const { return raw_ ? raw_->component() : 0; }
    qint64 Connection::bytesAvailable() const { return ByteStream::bytesAvailable(); }
    qint64 Connection::bytesToWrite() const { return raw_ ? raw_->bytesToWrite() : 0; }

    bool Connection::hasPendingDatagrams() const
    {
        return bool(features() & TransportFeature::MessageOriented) && bytesAvailable() > 0;
    }

    QNetworkDatagram Connection::readDatagram(qint64 maxSize)
    {
        const qint64 available = bytesAvailable();
        if (maxSize < 0 || maxSize > available)
            maxSize = available;
        return QNetworkDatagram(takeRead(int(qMin(maxSize, qint64(std::numeric_limits<int>::max()))));
    }

    bool Connection::writeDatagram(const QNetworkDatagram &datagram)
    {
        return write(datagram.data()) == datagram.data().size();
    }

    qint64 Connection::readDataInternal(char *data, qint64 maxSize)
    {
        return ByteStream::readData(data, maxSize);
    }

    bool Connection::writeWire(const QByteArray &data)
    {
        if (!raw_)
            return false;
        if (data.isEmpty())
            return true;
        if (raw_->features() & TransportFeature::MessageOriented)
            return raw_->writeDatagram(QNetworkDatagram(data));
        return raw_->write(data) == data.size();
    }

    qint64 Connection::writeData(const char *data, qint64 maxSize)
    {
        if (!sender_ || !cipher_ || failed_ || finished_ || maxSize < 0
            || quint64(maxSize) > plainSize_ - plainWritten_
            || maxSize > std::numeric_limits<int>::max()) {
            fail(QStringLiteral("JET write exceeds the negotiated plaintext length"));
            return -1;
        }
        if (!maxSize)
            return 0;
        const auto encrypted = cipher_->process(QByteArray(data, int(maxSize)));
        if (encrypted.size() != maxSize || !writeWire(encrypted)) {
            fail(QStringLiteral("Failed writing encrypted JET data"));
            return -1;
        }
        plainWritten_ += quint64(maxSize);
        if (plainWritten_ == plainSize_)
            finishSending();
        return maxSize;
    }

    void Connection::finishSending()
    {
        if (failed_ || finished_ || !cipher_)
            return;
        const auto tag = cipher_->finish();
        if (tag.size() != StreamCipher::TagSize || !writeWire(tag)) {
            fail(QStringLiteral("Failed to finish the authenticated JET stream"));
            return;
        }
        finished_ = true;
    }

    void Connection::pump()
    {
        if (sender_ || !cipher_ || failed_ || !raw_)
            return;
        const auto expected = plainSize_ + quint64(StreamCipher::TagSize);
        while (!failed_ && wireRead_ < expected
               && (raw_->bytesAvailable() > 0 || raw_->hasPendingDatagrams())) {
            QByteArray chunk;
            if (raw_->features() & TransportFeature::MessageOriented)
                chunk = raw_->readDatagram().data();
            else
                chunk = raw_->read(qint64(qMin(quint64(65536), expected - wireRead_)));
            if (chunk.isEmpty())
                break;
            if (quint64(chunk.size()) > expected - wireRead_) {
                fail(QStringLiteral("JET payload exceeds its negotiated length"));
                return;
            }
            wireRead_ += quint64(chunk.size());
            auto plain = cipher_->process(chunk);
            if (wireRead_ == expected) {
                if (!cipher_->verify()) {
                    fail(QStringLiteral("JET authentication tag is invalid"));
                    return;
                }
                verified_ = true;
                finished_ = true;
            }
            if (!plain.isEmpty()) {
                appendRead(plain);
                emit readyRead();
            }
        }
    }

    void Connection::fail(const QString &message)
    {
        if (failed_)
            return;
        failed_ = true;
        setError(ByteStream::ErrRead, message);
    }

    void Connection::close()
    {
        if (raw_)
            raw_->close();
        QIODevice::close();
    }
}}} // namespace XMPP::Jingle::JET
