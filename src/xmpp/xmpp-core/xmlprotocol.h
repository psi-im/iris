/*
 * xmlprotocol.h - state machine for 'xmpp-like' protocols
 * Copyright (C) 2004  Justin Karneges
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

#ifndef XMLPROTOCOL_H
#define XMLPROTOCOL_H

#include "parser.h"

#include <QList>
#include <QObject>
#include <qdom.h>

#define NS_XML "http://www.w3.org/XML/1998/namespace"

namespace XMPP {
    class XmlProtocol : public QObject
    {
    public:
        enum Need {
            NNotify,      // need a data send and/or recv update
            NCustom = 10
        };
        enum Event {
            EError,       // unrecoverable error, see errorCode for details
            ESend,        // data needs to be sent, use takeOutgoingData()
            ERecvOpen,    // breakpoint after root element open tag is received
            EPeerClosed,  // root element close tag received
            EClosed,      // finished closing
            ESMConnTimeout,  // absence of responses to <r/> query
            ESMResumeFailed, // failed to resume sm session
            ECustom = 10
        };
        enum Error {
            ErrParse,     // there was an error parsing the xml
            ErrCustom = 10
        };
        enum Notify {
            NSend = 0x01, // need to know if data has been written
            NRecv = 0x02,  // need incoming data
            NTimeout = 0x04 // need to know when time passed
        };

        XmlProtocol();
        virtual ~XmlProtocol();

        virtual void reset();

        // byte I/O for the stream
        void addIncomingData(const QByteArray &);
        QByteArray takeOutgoingData();
        void outgoingDataWritten(int);

        // advance the state machine
        bool processStep();

        // set these before returning from a step
        int need = 0, event = 0, errorCode = 0, notify = 0, timeout_sec = 0;

        inline bool isIncoming() const { return incoming; }
        QString xmlEncoding() const;
        QString elementToString(const QDomElement &e, bool clip=false);

        class TransferItem
        {
        public:
            TransferItem();
            TransferItem(const QString &str, bool sent, bool external=false);
            TransferItem(const QDomElement &elem, bool sent, bool external=false);

            bool isSent; // else, received
            bool isString; // else, is element
            bool isExternal; // not owned by protocol
            QString str;
            QDomElement elem;
        };
        QList<TransferItem> transferItemList;
        void setIncomingAsExternal();

    protected:
        virtual QDomElement docElement()=0;
        virtual void handleDocOpen(const Parser::Event &pe)=0;
        virtual bool handleError()=0;
        virtual bool handleCloseFinished()=0;
        virtual bool stepAdvancesParser() const=0;
        virtual bool stepRequiresElement() const;
        virtual bool doStep(const QDomElement &e)=0;
        virtual void itemWritten(int id, int size);

        // 'debug'
        virtual void stringSend(const QString &s);
        virtual void stringRecv(const QString &s);
        virtual void elementSend(const QDomElement &e);
        virtual void elementRecv(const QDomElement &e);

        void startConnect();
        void startAccept();
        bool close();
        int writeString(const QString &s, int id, bool external);
        int writeElement(const QDomElement &e, int id, bool external, bool clip=false, bool urgent = false);
        QByteArray resetStream();

    private:
        enum { SendOpen, RecvOpen, Open, Closing };
        class TrackItem
        {
        public:
            enum Type { Raw, Close, Custom };
            int type, id, size;
        };

        bool incoming;
        QDomDocument elemDoc;
        QDomElement elem;
        QString tagOpen;
        QString tagClose;
        int state = 0;
        bool peerClosed;
        bool closeWritten;

        Parser xml;
        QByteArray outDataNormal;
        QByteArray outDataUrgent;
        QList<TrackItem> trackQueueNormal;
        QList<TrackItem> trackQueueUrgent;

        void init();
        int internalWriteData(const QByteArray &a, TrackItem::Type t, int id=-1, bool urgent = false);
        int internalWriteString(const QString &s, TrackItem::Type t, int id=-1, bool urgent = false);
        int processTrackQueue(QList<TrackItem> &queue, int bytes);
        void sendTagOpen();
        void sendTagClose();
        bool baseStep(const Parser::Event &pe);
    };
} // namespace XMPP

#endif // XMLPROTOCOL_H
