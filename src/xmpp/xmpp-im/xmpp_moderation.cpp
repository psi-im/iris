/*
 * xmpp_moderation.cpp - XEP-0425 MUC moderated message retraction requests
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "xmpp_moderation.h"

#include "xmpp_xmlcommon.h"

#include <QDomDocument>

namespace XMPP {

ModeratedRetractionTask::ModeratedRetractionTask(Task *parent) : Task(parent) { }

void ModeratedRetractionTask::request(const Jid &room, const Message::StanzaId &target,
                                     const QString &reason)
{
    room_ = room;
    targetId_ = target.id;
    reason_ = reason;
    valid_ = room.isValid() && !room.isEmpty() && room.resource().isEmpty()
        && target.by.isValid() && !target.by.isEmpty() && target.by.resource().isEmpty()
        && room == target.by && !targetId_.isEmpty();
}

void ModeratedRetractionTask::onGo()
{
    if (!valid_) {
        setError(1, QStringLiteral("Moderated retraction requires a MUC-issued stanza ID and bare room JID"));
        return;
    }

    auto iq = createIQ(doc(), QStringLiteral("set"), room_.bare(), id());
    auto moderate = doc()->createElementNS(QStringLiteral("urn:xmpp:message-moderate:1"),
                                          QStringLiteral("moderate"));
    moderate.setAttribute(QStringLiteral("id"), targetId_);
    moderate.appendChild(doc()->createElementNS(QStringLiteral("urn:xmpp:message-retract:1"),
                                                QStringLiteral("retract")));
    if (!reason_.isEmpty())
        moderate.appendChild(textTagNS(doc(), QStringLiteral("urn:xmpp:message-moderate:1"),
                                       QStringLiteral("reason"), reason_));
    iq.appendChild(moderate);
    send(iq);
}

bool ModeratedRetractionTask::take(const QDomElement &stanza)
{
    if (!valid_ || !iqVerify(stanza, room_, id()))
        return false;
    if (stanza.attribute(QStringLiteral("type")) == QLatin1String("result"))
        setSuccess();
    else
        setError(stanza);
    return true;
}

} // namespace XMPP
