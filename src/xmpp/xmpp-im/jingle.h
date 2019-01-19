#ifndef JINGLE_H
#define JINGLE_H

#include <QSharedDataPointer>

class QDomElement;
class QDomDocument;

#define JINGLE_NS "urn:xmpp:jingle:1"

namespace XMPP {
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

    inline Jingle(){}
    Jingle(const QDomElement &e);
    QDomElement element(QDomDocument *doc) const;
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

    inline Reason(){}
    Reason(const QDomElement &el);
    inline bool isValid() const { return d != nullptr; }
    Condition condition() const;
    void setCondition(Condition cond);
    QString text() const;
    void setText(const QString &text);

    QDomElement element(QDomDocument *doc) const;

private:
    Private *ensureD();

    QSharedDataPointer<Private> d;
};

class Content
{
public:
    enum class Creator {
        Initiator,
        Responder
    };

    enum class Senders {
        None,
        Both,
        Initiator,
        Responder
    };

    inline Content(){}
    Content(const QDomElement &content);
    inline bool isValid() const { return d != nullptr; }
    QDomElement element(QDomDocument *doc) const;
private:
    class Private;
    Private *ensureD();
    QSharedDataPointer<Private> d;
};

} // namespace Jingle
} // namespace XMPP

#endif // JINGLE_H
