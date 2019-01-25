/*
 * jignle.h - General purpose Jingle
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

#ifndef JINGLE_H
#define JINGLE_H

#include "xmpp_hash.h"

#include <QSharedDataPointer>

class QDomElement;
class QDomDocument;

#define JINGLE_NS "urn:xmpp:jingle:1"
#define JINGLE_FT_NS "urn:xmpp:jingle:apps:file-transfer:5"

namespace XMPP {
class Client;

namespace Jingle {

class Jingle
{
public:
    enum Action {
        NoAction, // non-standard, just a default
        ContentAccept,
        ContentAdd,
        ContentModify,
        ContentReject,
        ContentRemove,
        DescriptionInfo,
        SecurityInfo,
        SessionAccept,
        SessionInfo,
        SessionInitiate,
        SessionTerminate,
        TransportAccept,
        TransportInfo,
        TransportReject,
        TransportReplace
    };

    Jingle();
    Jingle(const QDomElement &e);
    Jingle(const Jingle &);
    ~Jingle();
    QDomElement toXml(QDomDocument *doc) const;
private:
    class Private;
    QSharedDataPointer<Private> d;
    Jingle::Private *ensureD();
};

class Reason {
    class Private;
public:
    enum Condition
    {
        NoReason = 0, // non-standard, just a default
        AlternativeSession,
        Busy,
        Cancel,
        ConnectivityError,
        Decline,
        Expired,
        FailedApplication,
        FailedTransport,
        GeneralError,
        Gone,
        IncompatibleParameters,
        MediaError,
        SecurityError,
        Success,
        Timeout,
        UnsupportedApplications,
        UnsupportedTransports
    };

    Reason();
    Reason(const QDomElement &el);
    Reason(const Reason &other);
    inline bool isValid() const { return d != nullptr; }
    Condition condition() const;
    void setCondition(Condition cond);
    QString text() const;
    void setText(const QString &text);

    QDomElement toXml(QDomDocument *doc) const;

private:
    Private *ensureD();

    QSharedDataPointer<Private> d;
};

class ContentBase {
public:
    enum class Creator {
        NoCreator, // not standard, just a default
        Initiator,
        Responder
    };

    enum class Senders {
        Both, // it's default
        None,
        Initiator,
        Responder
    };

    inline ContentBase(){}
    ContentBase(const QDomElement &el);

    inline bool isValid() const { return creator != Creator::NoCreator && !name.isEmpty(); }
protected:
    QDomElement toXml(QDomDocument *doc, const char *tagName) const;
    static Creator creatorAttr(const QDomElement &el);
    static bool setCreatorAttr(QDomElement &el, Creator creator);

    Creator creator = Creator::NoCreator;
    QString name;
    Senders senders = Senders::Both;
    QString disposition; // default "session"
};

class Content : public ContentBase // TODO that's somewhat wrong mixing pimpl with this base
{
public:

    inline Content(){}
    Content(const QDomElement &content);
    QDomElement toXml(QDomDocument *doc) const;
};

class Manager;
class Session : public QObject
{
    Q_OBJECT
public:
    Session(Manager *manager);
    ~Session();

    void initiate(const Content &content);
private:
    class Private;
    QScopedPointer<Private> d;
};

class Application : public QObject
{
    Q_OBJECT
public:
    Application(Client *client);
    virtual ~Application();

    Client *client() const;
    virtual void incomingSession(Session *session, const QDomElement &contentEl) = 0;
private:
    class Private;
    QScopedPointer<Private> d;
};

class Transport
{

};

class Manager : public QObject
{
    Q_OBJECT

	static const int MaxSessions = 1000; //1000? just to have some limit

public:
	explicit Manager(XMPP::Client *client = 0);
	~Manager();

	XMPP::Client* client() const;
	//Session* sessionInitiate(const Jid &to, const QDomElement &description, const QDomElement &transport);
	// TODO void setRedirection(const Jid &to);

	void registerApp(const QString &ns, Application *app);

	Session* newSession(const Jid &j);

private:
    class Private;
	QScopedPointer<Private> d;
};

class Description
{
public:
    enum class Type {
        Unrecognized, // non-standard, just a default
        FileTransfer, // urn:xmpp:jingle:apps:file-transfer:5
    };
private:
    class Private;
    Private *ensureD();
    QSharedDataPointer<Private> d;
};

} // namespace Jingle
} // namespace XMPP

#endif // JINGLE_H
