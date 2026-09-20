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

#include <QByteArray>
#include <QList>
#include <QString>

class QDomDocument;
class QDomElement;

namespace XMPP { namespace Jingle {

class IRIS_EXPORT MessageInitiation {
public:
    enum class Action { None, Propose, Ringing, Proceed, Reject, Retract, Finish };

    struct Description {
        QString    ns;
        QString    media;
        QByteArray xml;

        bool operator==(const Description &other) const
        {
            return ns == other.ns && media == other.media && xml == other.xml;
        }
    };

    MessageInitiation();
    MessageInitiation(Action action, const QString &id);

    static const QString &ns();
    static MessageInitiation fromXml(const QDomElement &element);

    bool    isValid() const;
    Action  action() const;
    QString id() const;

    const QList<Description> &descriptions() const;
    void addDescription(const QString &descriptionNs, const QString &media = QString(),
                        const QByteArray &xml = QByteArray());
    void setDescriptions(const QList<Description> &descriptions);

    QString reasonCondition() const;
    QString reasonText() const;
    void    setReason(const QString &condition, const QString &text = QString());

    bool tieBreak() const;
    void setTieBreak(bool enabled);

    QString migratedTo() const;
    void setMigratedTo(const QString &id);

    const QList<QByteArray> &extensions() const;
    void addExtension(const QByteArray &xml);

    QDomElement toXml(QDomDocument *doc) const;

private:
    Action             action_ = Action::None;
    QString            id_;
    QList<Description> descriptions_;
    QString            reasonCondition_;
    QString            reasonText_;
    bool               tieBreak_ = false;
    QString            migratedTo_;
    QList<QByteArray>  extensions_;
};

}} // namespace XMPP::Jingle

#endif // XMPP_JINGLEMESSAGE_H
