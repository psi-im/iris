/*
 * xmlprotocol.cpp - state machine for 'xmpp-like' protocols
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

#include "xmlprotocol.h"

#include "bytestream.h"

#include <QByteArray>
#include <QList>
#include <QTextStream>

using namespace XMPP;

// stripExtraNS
//
// This function removes namespace information from various nodes for
// display purposes only (the element is pretty much useless for processing
// after this).  We do this because QXml is a bit overzealous about outputting
// redundant namespaces.
static QDomElement stripExtraNS(const QDomElement &e)
{
    // find closest parent with a namespace
    QDomNode par = e.parentNode();
    while(!par.isNull() && par.namespaceURI().isNull())
        par = par.parentNode();
    bool noShowNS = false;
    if(!par.isNull() && par.namespaceURI() == e.namespaceURI())
        noShowNS = true;

    // build qName (prefix:localName)
    QString qName;
    if(!e.prefix().isEmpty())
        qName = e.prefix() + ':' + e.localName();
    else
        qName = e.tagName();

    QDomElement i;
    int x;
    if(noShowNS)
        i = e.ownerDocument().createElement(qName);
    else
        i = e.ownerDocument().createElementNS(e.namespaceURI(), qName);

    // copy attributes
    QDomNamedNodeMap al = e.attributes();
    for(x = 0; x < al.count(); ++x) {
        QDomAttr a = al.item(x).cloneNode().toAttr();

        // don't show xml namespace
        if(a.namespaceURI() == NS_XML)
            i.setAttribute(QString("xml:") + a.name(), a.value());
        else
            i.setAttributeNodeNS(a);
    }

    // copy children
    QDomNodeList nl = e.childNodes();
    for(x = 0; x < nl.count(); ++x) {
        QDomNode n = nl.item(x);
        if(n.isElement())
            i.appendChild(stripExtraNS(n.toElement()));
        else
            i.appendChild(n.cloneNode());
    }
    return i;
}

// xmlToString
//
// This function converts a QDomElement into a QString, using stripExtraNS
// to make it pretty.
static QString xmlToString(const QDomElement &e, const QString &fakeNS, const QString &fakeQName, bool clip)
{
    QDomElement i = e.cloneNode().toElement();

    // It seems QDom can only have one namespace attribute at a time (see docElement 'HACK').
    // Fortunately we only need one kind depending on the input, so it is specified here.
    QDomElement fake = e.ownerDocument().createElementNS(fakeNS, fakeQName);
    fake.appendChild(i);
    fake = stripExtraNS(fake);
    QString out;
    {
        QTextStream ts(&out, QIODevice::WriteOnly);
        // NOTE: Workaround for bug in QtXML https://bugreports.qt.io/browse/QTBUG-25291 (Qt4 only):
        // Qt by default convert low surrogate to XML notation &#x....; and let high in binary!
        //
        // Qt is calling encode function per UTF-16 codepoint, which means that high and low
        // surrogate pairs are encoded separately. So all encoding except UTF-16 will leads
        // to damaged Unicode characters above 0xFFFF. Internal QString encoding is UTF-16
        // so this should be safe as QString still contains valid Unicode characters.
        ts.setCodec("UTF-16");
        fake.firstChild().save(ts, 0);
    }
    // 'clip' means to remove any unwanted (and unneeded) characters, such as a trailing newline
    if(clip) {
        int n = out.lastIndexOf('>');
        out.truncate(n+1);
    }
    return out;
}

// createRootXmlTags
//
// This function creates three QStrings, one being an <?xml .. ?> processing
// instruction, and the others being the opening and closing tags of an
// element, <foo> and </foo>.  This basically allows us to get the raw XML
// text needed to open/close an XML stream, without resorting to generating
// the XML ourselves.  This function uses QDom to do the generation, which
// ensures proper encoding and entity output.
static void createRootXmlTags(const QDomElement &root, QString *xmlHeader, QString *tagOpen, QString *tagClose)
{
    QDomElement e = root.cloneNode(false).toElement();

    // insert a dummy element to ensure open and closing tags are generated
    QDomElement dummy = e.ownerDocument().createElement("dummy");
    e.appendChild(dummy);

    // convert to xml->text
    QString str;
    {
        QTextStream ts(&str, QIODevice::WriteOnly);
        e.save(ts, 0);
    }

    // parse the tags out
    int n = str.indexOf('<');
    int n2 = str.indexOf('>', n);
    ++n2;
    *tagOpen = str.mid(n, n2-n);
    n2 = str.lastIndexOf('>');
    n = str.lastIndexOf('<');
    ++n2;
    *tagClose = str.mid(n, n2-n);

    // generate a nice xml processing header
    *xmlHeader = "<?xml version=\"1.0\"?>";
}

// w3c xml spec:
// [2] Char ::= #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] | [#x10000-#x10FFFF]
static inline bool validChar(const quint32 ch)
{
    return ch == 0x9 || ch == 0xA || ch == 0xD
            || (ch >= 0x20 && ch <= 0xD7FF)
            || (ch >= 0xE000 && ch <= 0xFFFD)
            || (ch >= 0x10000 && ch <= 0x10FFFF);
}

static inline bool lowSurrogate(const quint32 ch)
{
    return  ch >= 0xDC00 && ch <= 0xDFFF;
}

static inline bool highSurrogate(const quint32 ch)
{
    return  ch >= 0xD800 && ch <= 0xDBFF;
}

// force encoding of '>'.  this function is needed for XMPP-Core, which
//  requires the '>' character to be encoded as "&gt;" even though this is
//  not required by the XML spec.
// Also remove chars that are ouside the allowed range for XML (see validChar)
//  and invalid surrogate pairs
static QString sanitizeForStream(const QString &in)
{
    QString out;
    bool intag = false;
    bool inquote = false;
    QChar quotechar;
    int inlength = in.length();
    for(int n = 0; n < inlength; ++n)
    {
        QChar c = in[n];
        bool escape = false;
        if(c == '<')
        {
            intag = true;
        }
        else if(c == '>')
        {
            if(inquote) {
                escape = true;
            } else if(!intag) {
                escape = true;
            } else {
                intag = false;
            }
        }
        else if(c == '\'' || c == '\"')
        {
            if(intag)
            {
                if(!inquote)
                {
                    inquote = true;
                    quotechar = c;
                }
                else
                {
                    if(quotechar == c) {
                        inquote = false;
                    }
                }
            }
        }

        if(escape) {
            out += "&gt;";
         } else {
            // don't silently drop invalid chars in element or attribute names,
            // because that's something that should not happen.
            if (intag && (!inquote)) {
                out += c;
            } else if (validChar(c.unicode()))  {
                out += c;
            } else if (highSurrogate(c.unicode()) && (n+1 < inlength) && lowSurrogate(in[n+1].unicode())) {
                //uint unicode = (c.unicode() & 0x3FF) << 10 | in[n+1].unicode() & 0x3FF + 0x10000;
                // we don't need to recheck this, because 0x10000 <= unicode <= 0x100000 is always true
                out += c;
                out += in[n+1];
                ++n;
            } else {
                qDebug("Dropping invalid XML char U+%04x",c.unicode());
            }
        }
    }
    return out;
}

//----------------------------------------------------------------------------
// Protocol
//----------------------------------------------------------------------------
XmlProtocol::TransferItem::TransferItem()
{
}

XmlProtocol::TransferItem::TransferItem(const QString &_str, bool sent, bool external)
    : isSent(sent)
    , isString(true)
    , isExternal(external)
    , str(_str)
{
}

XmlProtocol::TransferItem::TransferItem(const QDomElement &_elem, bool sent, bool external)
    : isSent(sent)
    , isString(false)
    , isExternal(external)
    , elem(_elem)
{
}

XmlProtocol::XmlProtocol()
{
    init();
}

XmlProtocol::~XmlProtocol()
{
}

void XmlProtocol::init()
{
    incoming = false;
    peerClosed = false;
    closeWritten = false;
}

void XmlProtocol::reset()
{
    init();

    elem = QDomElement();
    elemDoc = QDomDocument();
    tagOpen = QString();
    tagClose = QString();
    xml.reset();
    outDataNormal.resize(0);
    outDataUrgent.resize(0);
    trackQueueNormal.clear();
    trackQueueUrgent.clear();
    transferItemList.clear();
}

void XmlProtocol::addIncomingData(const QByteArray &a)
{
    xml.appendData(a);
}

QByteArray XmlProtocol::takeOutgoingData()
{
    if (!outDataUrgent.isEmpty()) {
        QByteArray a = outDataUrgent;
        outDataUrgent.resize(0);
        return a;
    }
    QByteArray a = outDataNormal;
    outDataNormal.resize(0);
    return a;
}

void XmlProtocol::outgoingDataWritten(int bytes)
{
    int b = processTrackQueue(trackQueueUrgent, bytes);
    if (b > 0)
        processTrackQueue(trackQueueNormal, b);
}

bool XmlProtocol::processStep()
{
    Parser::Event pe;
    notify = 0;
    transferItemList.clear();

    if(state != Closing && (state == RecvOpen || stepAdvancesParser())) {
        // if we get here, then it's because we're in some step that advances the parser
        pe = xml.readNext();
        if(!pe.isNull()) {
            // note: error/close events should be handled for ALL steps, so do them here
            switch(pe.type()) {
                case Parser::Event::DocumentOpen: {
                    transferItemList += TransferItem(pe.actualString(), false);

                    //stringRecv(pe.actualString());
                    break;
                }
                case Parser::Event::DocumentClose: {
                    transferItemList += TransferItem(pe.actualString(), false);

                    //stringRecv(pe.actualString());
                    if(incoming) {
                        sendTagClose();
                        event = ESend;
                        peerClosed = true;
                        state = Closing;
                    }
                    else {
                        event = EPeerClosed;
                    }
                    return true;
                }
                case Parser::Event::Element: {
                    QDomElement e = elemDoc.importNode(pe.element(),true).toElement();
                    transferItemList += TransferItem(e, false);

                    //elementRecv(pe.element());
                    break;
                }
                case Parser::Event::Error: {
                    if(incoming) {
                        // If we get a parse error during the initial element exchange,
                        // flip immediately into 'open' mode so that we can report an error.
                        if(state == RecvOpen) {
                            sendTagOpen();
                            state = Open;
                        }
                        return handleError();
                    }
                    else {
                        event = EError;
                        errorCode = ErrParse;
                        return true;
                    }
                }
            }
        }
        else {
            if(state == RecvOpen || stepRequiresElement()) {
                need = NNotify;
                notify |= NRecv;
                return false;
            }
        }
    }

    return baseStep(pe);
}

QString XmlProtocol::xmlEncoding() const
{
    return xml.encoding();
}

QString XmlProtocol::elementToString(const QDomElement &e, bool clip)
{
    if(elem.isNull())
        elem = elemDoc.importNode(docElement(), true).toElement();

    // Determine the appropriate 'fakeNS' to use
    QString ns;

    // first, check root namespace
    QString pre = e.prefix();
    if(pre.isNull())
        pre = "";
    if(pre == elem.prefix()) {
        ns = elem.namespaceURI();
    }
    else {
        // scan the root attributes for 'xmlns' (oh joyous hacks)
        QDomNamedNodeMap al = elem.attributes();
        int n;
        for(n = 0; n < al.count(); ++n) {
            QDomAttr a = al.item(n).toAttr();
            QString s = a.name();
            int x = s.indexOf(':');
            if(x != -1)
                s = s.mid(x+1);
            else
                s = "";
            if(pre == s) {
                ns = a.value();
                break;
            }
        }
        if(n >= al.count()) {
            // if we get here, then no appropriate ns was found.  use root then..
            ns = elem.namespaceURI();
        }
    }

    // build qName
    QString qn;
    if(!elem.prefix().isEmpty())
        qn = elem.prefix() + ':';
    qn += elem.localName();

    // make the string
    return sanitizeForStream(xmlToString(e, ns, qn, clip));
}

bool XmlProtocol::stepRequiresElement() const
{
    // default returns false
    return false;
}

void XmlProtocol::itemWritten(int, int)
{
    // default does nothing
}

void XmlProtocol::stringSend(const QString &)
{
    // default does nothing
}

void XmlProtocol::stringRecv(const QString &)
{
    // default does nothing
}

void XmlProtocol::elementSend(const QDomElement &)
{
    // default does nothing
}

void XmlProtocol::elementRecv(const QDomElement &)
{
    // default does nothing
}

void XmlProtocol::startConnect()
{
    incoming = false;
    state = SendOpen;
}

void XmlProtocol::startAccept()
{
    incoming = true;
    state = RecvOpen;
}

bool XmlProtocol::close()
{
    sendTagClose();
    event = ESend;
    state = Closing;
    return true;
}

int XmlProtocol::writeString(const QString &s, int id, bool external)
{
    transferItemList += TransferItem(s, true, external);
    return internalWriteString(s, TrackItem::Custom, id);
}

int XmlProtocol::writeElement(const QDomElement &e, int id, bool external, bool clip, bool urgent)
{
    if(e.isNull())
        return 0;
    transferItemList += TransferItem(e, true, external);

    //elementSend(e);
    QString out = sanitizeForStream(elementToString(e, clip));
    return internalWriteString(out, TrackItem::Custom, id, urgent);
}

QByteArray XmlProtocol::resetStream()
{
    // reset the state
    if(incoming)
        state = RecvOpen;
    else
        state = SendOpen;

    // grab unprocessed data before resetting
    QByteArray spare = xml.unprocessed();
    xml.reset();
    return spare;
}

int XmlProtocol::internalWriteData(const QByteArray &a, TrackItem::Type t, int id, bool urgent)
{
    TrackItem i;
    i.type = t;
    i.id = id;
    i.size = a.size();

    if (urgent) {
        trackQueueUrgent += i;
        outDataUrgent += a;
    }
    else {
        trackQueueNormal += i;
        outDataNormal += a;
    }
    return a.size();
}

int XmlProtocol::internalWriteString(const QString &s, TrackItem::Type t, int id, bool urgent)
{
    QString out=sanitizeForStream(s);
    return internalWriteData(s.toUtf8(), t, id, urgent);
}

int XmlProtocol::processTrackQueue(QList<TrackItem> &queue, int bytes)
{
    for(QList<TrackItem>::Iterator it = queue.begin(); it != queue.end();) {
        TrackItem &i = *it;

        // enough bytes?
        if(bytes < i.size) {
            i.size -= bytes;
            bytes = 0;
            break;
        }
        int type = i.type;
        int id = i.id;
        int size = i.size;
        bytes -= i.size;
        it = queue.erase(it);

        if(type == TrackItem::Raw) {
            // do nothing
        }
        else if(type == TrackItem::Close) {
            closeWritten = true;
        }
        else if(type == TrackItem::Custom) {
            itemWritten(id, size);
        }
        if (bytes == 0)
            break;
    }
    return bytes;
}

void XmlProtocol::sendTagOpen()
{
    if(elem.isNull())
        elem = elemDoc.importNode(docElement(), true).toElement();

    QString xmlHeader;
    createRootXmlTags(elem, &xmlHeader, &tagOpen, &tagClose);

    QString s;
    s += xmlHeader + '\n';
    s += sanitizeForStream(tagOpen) + '\n';

    transferItemList += TransferItem(xmlHeader, true);
    transferItemList += TransferItem(tagOpen, true);

    //stringSend(xmlHeader);
    //stringSend(tagOpen);
    internalWriteString(s, TrackItem::Raw);
}

void XmlProtocol::sendTagClose()
{
    transferItemList += TransferItem(tagClose, true);

    //stringSend(tagClose);
    internalWriteString(tagClose, TrackItem::Close);
}

bool XmlProtocol::baseStep(const Parser::Event &pe)
{
    // Basic
    if(state == SendOpen) {
        sendTagOpen();
        event = ESend;
        if(incoming)
            state = Open;
        else
            state = RecvOpen;
        return true;
    }
    else if(state == RecvOpen) {
        if(incoming)
            state = SendOpen;
        else
            state = Open;

        // note: event will always be DocumentOpen here
        handleDocOpen(pe);
        event = ERecvOpen;
        return true;
    }
    else if(state == Open) {
        QDomElement e;
        if(pe.type() == Parser::Event::Element)
            e = pe.element();
        return doStep(e);
    }
    // Closing
    else {
        if(closeWritten) {
            if(peerClosed) {
                event = EPeerClosed;
                return true;
            }
            else
                return handleCloseFinished();
        }

        need = NNotify;
        notify = NSend;
        return false;
    }
}

void XmlProtocol::setIncomingAsExternal()
{
    for(QList<TransferItem>::Iterator it = transferItemList.begin(); it != transferItemList.end(); ++it) {
        TransferItem &i = *it;
        // look for elements received
        if(!i.isString && !i.isSent)
            i.isExternal = true;
    }
}

