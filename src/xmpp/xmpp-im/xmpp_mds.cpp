/*
 * xmpp_mds.cpp - XEP-0490 Message Displayed Synchronization wire helpers
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "xmpp_mds.h"

#include <QDomDocument>

namespace XMPP {
namespace {
constexpr auto MdsNs = "urn:xmpp:mds:displayed:0";
constexpr auto SidNs = "urn:xmpp:sid:0";

QString localName(const QDomElement &element)
{
    return element.localName().isEmpty() ? element.tagName().section(QLatin1Char(':'), -1)
                                         : element.localName();
}

bool isElement(const QDomElement &element, const char *local, const char *ns)
{
    return !element.isNull() && localName(element) == QLatin1String(local)
        && element.namespaceURI() == QLatin1String(ns);
}

bool isBareJid(const Jid &jid)
{
    return jid.isValid() && !jid.isEmpty() && jid.resource().isEmpty();
}
} // namespace

QString MessageDisplayedSync::node()
{
    return QLatin1String(MdsNs);
}

PubSubOptions MessageDisplayedSync::publishOptions()
{
    return {
        { QStringLiteral("pubsub#persist_items"), { QStringLiteral("true") } },
        { QStringLiteral("pubsub#max_items"), { QStringLiteral("max") } },
        { QStringLiteral("pubsub#send_last_published_item"), { QStringLiteral("never") } },
        { QStringLiteral("pubsub#access_model"), { QStringLiteral("whitelist") } }
    };
}

QDomElement MessageDisplayedSync::payload(QDomDocument &document, const Message::StanzaId &stanzaId)
{
    if (!isBareJid(stanzaId.by) || stanzaId.id.isEmpty())
        return {};

    auto displayed = document.createElementNS(QLatin1String(MdsNs), QStringLiteral("displayed"));
    auto sid = document.createElementNS(QLatin1String(SidNs), QStringLiteral("stanza-id"));
    sid.setAttribute(QStringLiteral("by"), stanzaId.by.bare());
    sid.setAttribute(QStringLiteral("id"), stanzaId.id);
    displayed.appendChild(sid);
    return displayed;
}

bool MessageDisplayedSync::parseItem(const PubSubItem &item, const Jid &expectedBy, State *state)
{
    if (!state || !isBareJid(expectedBy))
        return false;

    const Jid chat(item.id());
    if (!chat.isValid() || chat.isEmpty() || chat.full() != item.id())
        return false;

    const auto displayed = item.payload();
    if (!isElement(displayed, "displayed", MdsNs) || displayed.hasAttribute(QStringLiteral("id")))
        return false;

    const auto sid = displayed.firstChildElement();
    if (!isElement(sid, "stanza-id", SidNs) || !sid.nextSiblingElement().isNull())
        return false;

    const Jid by(sid.attribute(QStringLiteral("by")));
    const QString id = sid.attribute(QStringLiteral("id"));
    if (!isBareJid(by) || by != expectedBy || id.isEmpty())
        return false;

    State parsed;
    parsed.chat = chat;
    parsed.stanzaId = { by, id };
    *state = parsed;
    return true;
}

bool MessageDisplayedSync::isOwnNotification(const Jid &sender, const Jid &account, const PubSubEvent &event)
{
    return isBareJid(sender) && account.isValid() && !account.isEmpty()
        && sender.bare() == account.bare() && event.type() == PubSubEvent::Type::Items
        && event.node() == node();
}

} // namespace XMPP
