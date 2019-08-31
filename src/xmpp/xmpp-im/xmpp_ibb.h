/*
 * ibb.h - Inband bytestream
 * Copyright (C) 2001-2002  Justin Karneges
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

#ifndef XMPP_IBB_H
#define XMPP_IBB_H

#include "bytestream.h"
#include "im.h"
#include "xmpp_bytestream.h"
#include "xmpp_task.h"

#include <QDomElement>
#include <QList>
#include <QObject>

namespace XMPP {
    class Client;
    class IBBManager;

    class IBBData
    {
    public:
        IBBData() : seq(0) {}
        IBBData(const QString &sid, quint16 seq, const QByteArray &data)
            : sid(sid)
            , seq(seq)
            , data(data)
        {}

        IBBData& fromXml(const QDomElement &e);
        QDomElement toXml(QDomDocument *) const;

        QString sid;
        quint16 seq;
        QByteArray data;
    };

    // this is an IBB connection.  use it much like a qsocket
    class IBBConnection : public BSConnection
    {
        Q_OBJECT
    public:
        static const int PacketSize = 4096;

        enum { ErrRequest, ErrData };
        enum { Idle, Requesting, WaitingForAccept, Active };
        IBBConnection(IBBManager *);
        ~IBBConnection();

        void setPacketSize(int blockSize = IBBConnection::PacketSize);
        void connectToJid(const Jid &peer, const QString &sid);
        void accept();
        void close();

        int state() const;
        Jid peer() const;
        QString sid() const;
        BytestreamManager* manager() const;

        bool isOpen() const;

    protected:
        qint64 writeData(const char *data, qint64 maxSize);

    signals:
        void connected();

    private slots:
        void ibb_finished();
        void trySend();

    private:
        class Private;
        Private *d;

        void resetConnection(bool clear=false);

        friend class IBBManager;
        void waitForAccept(const Jid &peer, const QString &iq_id,
                           const QString &sid, int blockSize,
                           const QString &stanza);
        void takeIncomingData(const IBBData &ibbData);
        void setRemoteClosed();
    };

    typedef QList<IBBConnection*> IBBConnectionList;
    class IBBManager : public BytestreamManager
    {
        Q_OBJECT
    public:
        IBBManager(Client *);
        ~IBBManager();

        static const char* ns();
        Client *client() const;

        bool isAcceptableSID(const Jid &peer, const QString &sid) const;
        BSConnection *createConnection();
        IBBConnection *takeIncoming();

    public slots:
        void takeIncomingData(const Jid &from, const QString &id,
                              const IBBData &data, Stanza::Kind);

    protected:
        const char* sidPrefix() const;

    private slots:
        void ibb_incomingRequest(const Jid &from, const QString &id,
                                 const QString &sid, int blockSize,
                                 const QString &stanza);
        void ibb_closeRequest(const Jid &from, const QString &id,
                              const QString &sid);

    private:
        class Private;
        Private *d;

        friend class IBBConnection;
        IBBConnection *findConnection(const QString &sid, const Jid &peer="") const;
        void link(IBBConnection *);
        void unlink(IBBConnection *);
        void doAccept(IBBConnection *c, const QString &id);
        void doReject(IBBConnection *c, const QString &id,
                      Stanza::Error::ErrorCond cond, const QString &);
    };

    class JT_IBB : public Task
    {
        Q_OBJECT
    public:
        enum { ModeRequest, ModeSendData };
        JT_IBB(Task *, bool serve=false);
        ~JT_IBB();

        void request(const Jid &, const QString &sid, int blockSize = IBBConnection::PacketSize);
        void sendData(const Jid &, const IBBData &ibbData);
        void close(const Jid &, const QString &sid);
        void respondError(const Jid &, const QString &id,
                          Stanza::Error::ErrorCond cond, const QString &text = "");
        void respondAck(const Jid &to, const QString &id);

        void onGo();
        bool take(const QDomElement &);

        Jid jid() const;
        int mode() const;
        int bytesWritten() const;

    signals:
        void incomingRequest(const Jid &from, const QString &id,
                             const QString &sid, int blockSize,
                             const QString &stanza);
        void incomingData(const Jid &from, const QString &id,
                          const IBBData &data, Stanza::Kind);
        void closeRequest(const Jid &from, const QString &id, const QString &sid);

    private:
        class Private;
        Private *d;
    };
} // namespace XMPP

#endif // XMPP_IBB_H
