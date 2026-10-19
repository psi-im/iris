/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef XMPP_BOOKMARKS_H
#define XMPP_BOOKMARKS_H

#include <iris/xmpp-im/xmpp_pubsub.h>
#include <iris/xmpp_jid.h>

namespace XMPP {

// XEP-0402 is a PEP profile, not a second PubSub implementation. Reuse
// PubSubManager's items/publish/retract tasks and events. Keep the original
// XML to preserve extension data when editing a known conference field.
class IRIS_EXPORT NativeBookmark {
public:
    static QString node();
    static PubSubOptions privatePublishOptions();

    static NativeBookmark fromItem(const PubSubItem &item);
    bool isValid() const;
    Jid room() const;
    QString name() const;
    QString nick() const;
    QString password() const;
    bool autoJoin() const;
    void setName(const QString &);
    void setNick(const QString &);
    void setPassword(const QString &);
    void setAutoJoin(bool);
    PubSubItem toItem(QDomDocument &document) const;

    static NativeBookmark create(const Jid &room, const QString &name, bool autoJoin,
                                 const QString &nick = {}, const QString &password = {});
private:
    Jid room_;
    QDomDocument payloadDocument_;
    QDomElement payload_;
};
} // namespace XMPP
#endif // XMPP_BOOKMARKS_H
