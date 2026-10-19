/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "xmpp_bookmarks.h"

namespace XMPP {
namespace {
const QString ns = QStringLiteral("urn:xmpp:bookmarks:1");

QString childText(const QDomElement &parent, const QString &name)
{
    for (QDomElement e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        if (e.namespaceURI() == ns && (e.localName().isEmpty() ? e.tagName() : e.localName()) == name)
            return e.text();
    }
    return {};
}

void setChild(QDomElement &parent, const QString &name, const QString &value)
{
    QDomElement existing;
    for (auto e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
        if (e.namespaceURI() == ns && (e.localName().isEmpty() ? e.tagName() : e.localName()) == name) {
            existing = e;
            break;
        }
    if (value.isEmpty()) {
        if (!existing.isNull())
            parent.removeChild(existing);
    } else {
        if (existing.isNull()) {
            existing = parent.ownerDocument().createElementNS(ns, name);
            // Order matters under the schema: nick, password, extensions.
            const QDomElement extensions = parent.firstChildElement(QStringLiteral("extensions"));
            if (!extensions.isNull())
                parent.insertBefore(existing, extensions);
            else
                parent.appendChild(existing);
        }
        while (!existing.firstChild().isNull())
            existing.removeChild(existing.firstChild());
        existing.appendChild(parent.ownerDocument().createTextNode(value));
    }
}
} // namespace

QString NativeBookmark::node() { return ns; }

PubSubOptions NativeBookmark::privatePublishOptions()
{
    return { { QStringLiteral("pubsub#persist_items"), { QStringLiteral("true") } },
             { QStringLiteral("pubsub#max_items"), { QStringLiteral("max") } },
             { QStringLiteral("pubsub#send_last_published_item"), { QStringLiteral("never") } },
             { QStringLiteral("pubsub#access_model"), { QStringLiteral("whitelist") } } };
}

NativeBookmark NativeBookmark::fromItem(const PubSubItem &item)
{
    NativeBookmark result;
    const Jid room(item.id());
    const auto &payload = item.payload();
    if (!room.isValid() || room.isEmpty() || !room.resource().isEmpty()
        || payload.namespaceURI() != ns
        || (payload.localName().isEmpty() ? payload.tagName() : payload.localName()) != QLatin1String("conference"))
        return result;
    result.room_ = room;
    result.payload_ = result.payloadDocument_.importNode(payload, true).toElement();
    result.payloadDocument_.appendChild(result.payload_);
    return result;
}

NativeBookmark NativeBookmark::create(const Jid &room, const QString &name, bool autoJoin,
                                      const QString &nick, const QString &password)
{
    NativeBookmark result;
    if (!room.isValid() || room.isEmpty() || !room.resource().isEmpty())
        return result;
    result.room_ = room;
    result.payload_ = result.payloadDocument_.createElementNS(ns, QStringLiteral("conference"));
    result.payloadDocument_.appendChild(result.payload_);
    result.setName(name);
    result.setAutoJoin(autoJoin);
    result.setNick(nick);
    result.setPassword(password);
    return result;
}

bool NativeBookmark::isValid() const { return room_.isValid() && !room_.isEmpty() && !payload_.isNull(); }
Jid NativeBookmark::room() const { return room_; }
QString NativeBookmark::name() const { return payload_.attribute(QStringLiteral("name")); }
QString NativeBookmark::nick() const { return childText(payload_, QStringLiteral("nick")); }
QString NativeBookmark::password() const { return childText(payload_, QStringLiteral("password")); }
bool NativeBookmark::autoJoin() const
{
    const auto value = payload_.attribute(QStringLiteral("autojoin"));
    return value == QLatin1String("true") || value == QLatin1String("1");
}
void NativeBookmark::setName(const QString &name) { payload_.setAttribute(QStringLiteral("name"), name); }
void NativeBookmark::setAutoJoin(bool enabled)
{
    payload_.setAttribute(QStringLiteral("autojoin"), enabled ? QStringLiteral("true") : QStringLiteral("false"));
}
void NativeBookmark::setNick(const QString &nick) { setChild(payload_, QStringLiteral("nick"), nick); }
void NativeBookmark::setPassword(const QString &password) { setChild(payload_, QStringLiteral("password"), password); }

PubSubItem NativeBookmark::toItem(QDomDocument &document) const
{
    if (!isValid())
        return {};
    return PubSubItem(room_.bare(), document.importNode(payload_, true).toElement());
}
} // namespace XMPP
