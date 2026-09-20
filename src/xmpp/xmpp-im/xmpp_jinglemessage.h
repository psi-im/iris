/*
 * xmpp_jinglemessage.h - XEP-0353 Jingle Message Initiation value type
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#ifndef XMPP_JINGLEMESSAGE_H
#define XMPP_JINGLEMESSAGE_H

#include <iris/iris_export.h>

#include <QDomElement>
#include <QList>
#include <QSharedDataPointer>
#include <QString>

class QDomDocument;

namespace XMPP { namespace Jingle {

class IRIS_EXPORT MessageInitiation {
public:
    enum class Action { None, Propose, Ringing, Proceed, Reject, Retract, Finish };

    MessageInitiation();
    MessageInitiation(Action action, const QString &id);
    MessageInitiation(const MessageInitiation &);
    MessageInitiation &operator=(const MessageInitiation &);
    ~MessageInitiation();

    static const QString &ns();
    static MessageInitiation fromXml(const QDomElement &element);

    bool    isValid() const;
    Action  action() const;
    QString id() const;

    // JMI is application-agnostic. Each description is preserved as the
    // original foreign-namespace XML and owned by this value object.
    QList<QDomElement> descriptions() const;
    void setDescriptions(const QList<QDomElement> &descriptions);
    void addDescription(const QDomElement &description);
    void addDescription(const QString &applicationNamespace);

    QString reasonCondition() const;
    QString reasonText() const;
    void    setReason(const QString &condition, const QString &text = QString());

    bool tieBreak() const;
    void setTieBreak(bool enabled);

    QString migratedTo() const;
    void setMigratedTo(const QString &id);

    QList<QDomElement> extensions() const;
    void addExtension(const QDomElement &element);

    QDomElement toXml(QDomDocument *doc) const;

private:
    class Private;
    Private *ensureD();

    QSharedDataPointer<Private> d;
};

}} // namespace XMPP::Jingle

#endif // XMPP_JINGLEMESSAGE_H
