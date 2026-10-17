/*
 * xmpp_mamtask.cpp - XEP-0313 Message Archive Management
 * Copyright (C) 2024 mcneb10
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

#include "xmpp_mamtask.h"
#include "xmpp_features.h"

#include <QSet>

using namespace XMLHelper;
using namespace XMPP;

class MAMTask::Private {
public:
    int  mamPageSize = 10; // maximum messages per page
    int  mamMaxMessages = 0; // total cap; zero means unlimited
    int  messagesFetched = 0;
    bool flipPages = false;
    bool backwards = true;
    bool allowMUCArchives = true;
    Jid  j;
    Jid  archiveJid;
    Features archiveFeatures;
    bool complete = false;
    bool stable = true;
    bool active = false;
    bool collectResults = true;
    QSet<QString> visitedCursors;
    MAMTask           *q;
    QString            lastArchiveID;
    QString            fromID;
    QString            toID;
    QString            currentPageQueryID;
    QString            currentPageQueryIQID;
    QDateTime          from;
    QDateTime          to;
    QList<QDomElement> archive;

    void  getPage();
    XData makeMAMFilter();
};

MAMTask::MAMTask(Task *parent) : Task(parent)
{
    d = new Private;
    d->q = this;
    connect(this, &Task::finished, this, [this] { d->active = false; });
}
MAMTask::~MAMTask() { delete d; }

const QList<QDomElement> &MAMTask::archive() const { return d->archive; }
void MAMTask::setCollectResults(bool enabled) { d->collectResults = enabled; }
void MAMTask::setArchiveJid(const Jid &j) { d->archiveJid = j; }
void MAMTask::setArchiveFeatures(const Features &f) { d->archiveFeatures = f; }
bool MAMTask::isComplete() const { return d->complete; }
bool MAMTask::isStable() const { return d->stable; }

XData MAMTask::Private::makeMAMFilter()
{
    XData::FieldList fl;

    if (!j.isEmpty()) {
        XData::Field with;
        with.setType(XData::Field::Field_JidSingle);
        with.setVar(QLatin1String("with"));
        with.setValue(QStringList(j.full()));
        fl.append(with);
    }

    // The optional field is legal only when advertised by this archive.
    if (archiveJid.isEmpty() && archiveFeatures.test(QStringLiteral("urn:xmpp:mam:2#groupchat-field"))) {
        XData::Field includeGroupchat;
        includeGroupchat.setType(XData::Field::Field_Boolean);
        includeGroupchat.setVar(QLatin1String("include-groupchat"));
        includeGroupchat.setValue(QStringList(QLatin1String(allowMUCArchives ? "true" : "false")));
        fl.append(includeGroupchat);
    }

    if (from.isValid()) {
        XData::Field start;
        start.setType(XData::Field::Field_TextSingle);
        start.setVar(QLatin1String("start"));
        start.setValue(QStringList(from.toUTC().toString(Qt::ISODate)));
        fl.append(start);
    }

    if (to.isValid()) {
        XData::Field end;
        end.setType(XData::Field::Field_TextSingle);
        end.setVar(QLatin1String("end"));
        end.setValue(QStringList(to.toUTC().toString(Qt::ISODate)));
        fl.append(end);
    }

    if (!fromID.isEmpty()) {
        XData::Field start_id;
        start_id.setType(XData::Field::Field_TextSingle);
        start_id.setVar(QLatin1String("after-id"));
        start_id.setValue(QStringList(fromID));
        fl.append(start_id);
    }

    if (!toID.isEmpty()) {
        XData::Field end_id;
        end_id.setType(XData::Field::Field_TextSingle);
        end_id.setVar(QLatin1String("before-id"));
        end_id.setValue(QStringList(toID));
        fl.append(end_id);
    }

    XData x;
    x.setType(XData::Data_Submit);
    x.setFields(fl);
    x.setRegistrarType(XMPP_MAM_NAMESPACE);

    return x;
}

void MAMTask::Private::getPage()
{
    currentPageQueryIQID = q->genUniqueID();
    QDomElement iq       = createIQ(q->doc(), QLatin1String("set"), archiveJid.full(), currentPageQueryIQID);
    QDomElement query    = q->doc()->createElementNS(XMPP_MAM_NAMESPACE, QLatin1String("query"));
    currentPageQueryID   = q->genUniqueID();
    query.setAttribute(QLatin1String("queryid"), currentPageQueryID);
    XData x = makeMAMFilter();

    SubsetsClientManager rsm;
    const int remaining = mamMaxMessages > 0 ? mamMaxMessages - messagesFetched : mamPageSize;
    rsm.setMax(qMin(mamPageSize, remaining));

    if (lastArchiveID.isNull()) {
        if (backwards) {
            rsm.getLast();
        } else {
            rsm.getFirst();
        }
    } else {
        if (backwards) {
            rsm.setFirstID(lastArchiveID);
            rsm.getPrevious();
        } else {
            rsm.setLastID(lastArchiveID);
            rsm.getNext();
        }
    }

    query.appendChild(x.toXml(q->doc()));
    query.appendChild(rsm.makeQueryElement(q->doc()));
    if (flipPages && archiveFeatures.test(QStringLiteral("urn:xmpp:mam:2#extended")))
        query.appendChild(q->doc()->createElementNS(XMPP_MAM_NAMESPACE, QLatin1String("flip-page")));
    iq.appendChild(query);
    q->send(iq);
}

// Note: Set `j` to a resource if you just want to query that resource
// if you want to query all resources, set `j` to the bare JID

// Filter by time range
void MAMTask::get(const Jid &j, const QDateTime &from, const QDateTime &to, const bool allowMUCArchives,
                  int mamPageSize, int mamMaxMessages, bool flipPages, bool backwards)
{
    d->archive.clear();
    d->messagesFetched = 0;
    d->lastArchiveID = QString();
    d->visitedCursors.clear();
    d->complete = false;
    d->stable = true;
    d->active = false;

    d->j                = j;
    d->from             = from;
    d->to               = to;
    d->fromID           = QString();
    d->toID             = QString();
    d->allowMUCArchives = allowMUCArchives;
    d->mamPageSize      = qMax(1, mamPageSize);
    d->mamMaxMessages   = qMax(0, mamMaxMessages);
    d->flipPages        = flipPages;
    d->backwards        = backwards;
    d->q                = this;
}

// Filter by id range
void MAMTask::get(const Jid &j, const QString &fromID, const QString &toID, const bool allowMUCArchives,
                  int mamPageSize, int mamMaxMessages, bool flipPages, bool backwards)
{
    d->archive.clear();
    d->messagesFetched = 0;
    d->lastArchiveID = QString();
    d->visitedCursors.clear();
    d->complete = false;
    d->stable = true;
    d->active = false;

    d->j                = j;
    d->fromID           = fromID;
    d->toID             = toID;
    d->from             = QDateTime();
    d->to               = QDateTime();
    d->allowMUCArchives = allowMUCArchives;
    d->mamPageSize      = qMax(1, mamPageSize);
    d->mamMaxMessages   = qMax(0, mamMaxMessages);
    d->flipPages        = flipPages;
    d->backwards        = backwards;
}

void MAMTask::onGo()
{
    d->active = true;
    if ((!d->fromID.isEmpty() || !d->toID.isEmpty())
        && !d->archiveFeatures.test(QStringLiteral("urn:xmpp:mam:2#extended"))) {
        setError(1, QStringLiteral("MAM ID filters require urn:xmpp:mam:2#extended"));
        return;
    }
    d->getPage();
}

bool MAMTask::take(const QDomElement &x)
{
    if (!d->active)
        return false;
    // The final IQ is the page boundary. Sending the next query while the
    // server is still streaming <result/> messages can race with that IQ.
    if (iqVerify(x, d->archiveJid, d->currentPageQueryIQID)) {
        if (x.attribute(QLatin1String("type")) == QLatin1String("error")) {
            if (x.firstChildElement(QLatin1String("error")).isNull())
                setError(1, QStringLiteral("MAM IQ error without error payload"));
            else
                setError(x);
            return true;
        }
        if (x.attribute(QLatin1String("type")) != QLatin1String("result"))
            return false;

        QDomElement fin = x.firstChildElement(QLatin1String("fin"));
        if (fin.isNull() || fin.namespaceURI() != XMPP_MAM_NAMESPACE) {
            setError(1, QStringLiteral("Missing MAM <fin/> in query response"));
            return true;
        }

        d->complete = fin.attribute(QLatin1String("complete")) == QLatin1String("true");
        d->stable = d->stable && fin.attribute(QLatin1String("stable")) != QLatin1String("false");
        if (d->complete || (d->mamMaxMessages > 0 && d->messagesFetched >= d->mamMaxMessages)) {
            setSuccess();
            return true;
        }

        // XEP-0313 requires <fin/> to contain the RSM first/last IDs,
        // even when the server returned fewer items than we requested.
        const auto set = SubsetsClientManager::findElement(fin, true);
        if (set.isNull()) {
            setError(1, QStringLiteral("Missing RSM cursor in MAM <fin/>"));
            return true;
        }
        const auto edge = set.firstChildElement(d->backwards ? QLatin1String("first") : QLatin1String("last"));
        const QString cursor = edge.text();
        if (cursor.isEmpty() || d->visitedCursors.contains(cursor)) {
            setError(1, QStringLiteral("MAM pagination returned an empty or repeated cursor"));
            return true;
        }
        d->lastArchiveID = cursor;
        d->visitedCursors.insert(cursor);
        d->getPage();
        return true;
    }

    // Only results belonging to the current query are part of this task.
    if (x.tagName() != QLatin1String("message"))
        return false;
    const QDomElement result = x.firstChildElement(QLatin1String("result"));
    if (result.isNull() || result.namespaceURI() != XMPP_MAM_NAMESPACE
        || result.attribute(QLatin1String("queryid")) != d->currentPageQueryID)
        return false;

    // A matching queryid is not proof that the sender owns this archive.
    const Jid sender(x.attribute(QLatin1String("from")));
    if (!d->archiveJid.isEmpty()) {
        if (sender.isEmpty() || !sender.compare(d->archiveJid, false))
            return true; // discard forged result
    } else if (!sender.isEmpty() && !sender.compare(Jid(client()->jid().bare()), false)) {
        return true;
    }
    const QDomElement forwarded = result.firstChildElement(QLatin1String("forwarded"));
    if (forwarded.isNull() || forwarded.namespaceURI() != QLatin1String("urn:xmpp:forward:0")
        || result.attribute(QLatin1String("id")).isEmpty())
        return true;

    if (d->mamMaxMessages > 0 && d->messagesFetched >= d->mamMaxMessages) {
        // A server that sends more items than requested must not grow
        // the archive beyond the caller's explicit total limit.
        return true;
    }
    ++d->messagesFetched;
    if (d->collectResults)
        d->archive.append(result);
    emit resultReceived(result);
    return true;
}
