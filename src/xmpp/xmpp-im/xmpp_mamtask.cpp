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
#include "xmpp_client.h"
#include "xmpp_features.h"

#include <QSet>

#include <utility>
#include <variant>

using namespace XMLHelper;
using namespace XMPP;

namespace {
// Namespace-aware QDomElement::localName() handles prefixed server XML;
// retain the first matching local name so invalid namespaces are not skipped.
QDomElement directChildNamed(const QDomElement &parent, const QString &name)
{
    for (auto child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        const auto local = child.localName().isEmpty() ? child.tagName().section(QLatin1Char(':'), -1)
                                                        : child.localName();
        if (local == name)
            return child;
    }
    return {};
}
} // namespace

class MAMTask::Private {
public:
    // A query is configured once, before Task::go(). The filter kind makes
    // time and ID bounds mutually exclusive by construction.
    struct TimeRange {
        QDateTime from;
        QDateTime to;
    };
    struct IdRange {
        QString from;
        QString to;
    };
    struct Request {
        Jid with;
        std::variant<TimeRange, IdRange> range = TimeRange {};
        bool allowMUCArchives = true;
        int pageSize = 10;
        int maxMessages = 0;
        bool flipPages = false;
        bool backwards = true;

        static Request create(const Jid &with, bool allowMUCArchives, int pageSize,
                              int maxMessages, bool flipPages, bool backwards)
        {
            Request request;
            request.with = with;
            request.allowMUCArchives = allowMUCArchives;
            request.pageSize = qMax(1, pageSize);
            request.maxMessages = qMax(0, maxMessages);
            request.flipPages = flipPages;
            request.backwards = backwards;
            return request;
        }
    };

    // Mutable state for the *running* query, never touched by get().
    struct Progress {
        int fetched = 0;
        bool complete = false;
        bool stable = true;
        bool active = false;
        bool started = false; // Task::done() prevents a second execution
        QString cursor;
        QString fromId;
        QString toId;
        QString queryId;
        QString iqId;
        QSet<QString> visitedCursors;
        QList<QDomElement> archive;
    };

    Request request;
    Progress progress;
    // Per-archive capabilities and storage policy are configured separately.
    Jid archiveJid;
    Features archiveFeatures;
    bool collectResults = true;
};

MAMTask::MAMTask(Task *parent) : Task(parent), d(std::make_unique<Private>())
{
    connect(this, &Task::finished, this, [this] { d->progress.active = false; });
}
MAMTask::~MAMTask() = default;

const QList<QDomElement> &MAMTask::archive() const { return d->progress.archive; }
void MAMTask::setCollectResults(bool enabled)
{
    if (!d->progress.started)
        d->collectResults = enabled;
}
void MAMTask::setArchiveJid(const Jid &j)
{
    if (!d->progress.started)
        d->archiveJid = j;
}
void MAMTask::setArchiveFeatures(const Features &f)
{
    if (!d->progress.started)
        d->archiveFeatures = f;
}
bool MAMTask::isComplete() const { return d->progress.complete; }
bool MAMTask::isStable() const { return d->progress.stable; }

XData MAMTask::makeMAMFilter() const
{
    const auto &request = d->request;
    const auto &progress = d->progress;
    XData::FieldList fields;

    if (!request.with.isEmpty()) {
        XData::Field with;
        with.setType(XData::Field::Field_JidSingle);
        with.setVar(QLatin1String("with"));
        with.setValue(QStringList(request.with.full()));
        fields.append(with);
    }

    if (d->archiveJid.isEmpty()
        && d->archiveFeatures.test(QStringLiteral("urn:xmpp:mam:2#groupchat-field"))) {
        XData::Field includeGroupchat;
        includeGroupchat.setType(XData::Field::Field_Boolean);
        includeGroupchat.setVar(QLatin1String("include-groupchat"));
        includeGroupchat.setValue(QStringList(
            QLatin1String(request.allowMUCArchives ? "true" : "false")));
        fields.append(includeGroupchat);
    }

    const auto addField = [&fields](const QString &name, const QString &value) {
        if (value.isEmpty())
            return;
        XData::Field field;
        field.setType(XData::Field::Field_TextSingle);
        field.setVar(name);
        field.setValue(QStringList(value));
        fields.append(field);
    };

    if (const auto *range = std::get_if<Private::TimeRange>(&request.range)) {
        if (range->from.isValid())
            addField(QLatin1String("start"), range->from.toUTC().toString(Qt::ISODateWithMs));
        if (range->to.isValid())
            addField(QLatin1String("end"), range->to.toUTC().toString(Qt::ISODateWithMs));
    } else {
        // Effective boundaries can differ from the configured ID range when
        // a single boundary is expressed using baseline RSM.
        addField(QLatin1String("after-id"), progress.fromId);
        addField(QLatin1String("before-id"), progress.toId);
    }

    XData data;
    data.setType(XData::Data_Submit);
    data.setFields(fields);
    data.setRegistrarType(XMPP_MAM_NAMESPACE);
    return data;
}

bool MAMTask::isExpectedSender(const QDomElement &stanza) const
{
    const Jid sender(stanza.attribute(QLatin1String("from")));
    if (!d->archiveJid.isEmpty())
        return !sender.isEmpty() && sender.compare(d->archiveJid, true);
    return sender.isEmpty() || sender.compare(Jid(client()->jid().bare()), true);
}

void MAMTask::getPage()
{
    const auto &request = d->request;
    auto &progress = d->progress;

    progress.iqId = genUniqueID();
    QDomElement iq = createIQ(doc(), QLatin1String("set"), d->archiveJid.full(), progress.iqId);
    QDomElement query = doc()->createElementNS(XMPP_MAM_NAMESPACE, QLatin1String("query"));
    progress.queryId = genUniqueID();
    query.setAttribute(QLatin1String("queryid"), progress.queryId);

    SubsetsClientManager rsm;
    const int remaining = request.maxMessages > 0
        ? request.maxMessages - progress.fetched : request.pageSize;
    rsm.setMax(qMin(request.pageSize, remaining));
    if (progress.cursor.isNull()) {
        if (request.backwards)
            rsm.getLast();
        else
            rsm.getFirst();
    } else if (request.backwards) {
        rsm.setFirstID(progress.cursor);
        rsm.getPrevious();
    } else {
        rsm.setLastID(progress.cursor);
        rsm.getNext();
    }

    query.appendChild(makeMAMFilter().toXml(doc()));
    query.appendChild(rsm.makeQueryElement(doc()));
    if (request.flipPages && d->archiveFeatures.test(QStringLiteral("urn:xmpp:mam:2#extended")))
        query.appendChild(doc()->createElementNS(XMPP_MAM_NAMESPACE, QLatin1String("flip-page")));
    iq.appendChild(query);
    send(iq);
}

// The two get() overloads configure a one-shot task. They never reset an
// in-flight page, completion state, or the archive result buffer.
void MAMTask::get(const Jid &j, const QDateTime &from, const QDateTime &to, const bool allowMUCArchives,
                  int mamPageSize, int mamMaxMessages, bool flipPages, bool backwards)
{
    if (d->progress.started)
        return;
    auto request = Private::Request::create(
        j, allowMUCArchives, mamPageSize, mamMaxMessages, flipPages, backwards);
    request.range = Private::TimeRange { from, to };
    d->request = std::move(request);
}

void MAMTask::get(const Jid &j, const QString &fromID, const QString &toID, const bool allowMUCArchives,
                  int mamPageSize, int mamMaxMessages, bool flipPages, bool backwards)
{
    if (d->progress.started)
        return;
    auto request = Private::Request::create(
        j, allowMUCArchives, mamPageSize, mamMaxMessages, flipPages, backwards);
    request.range = Private::IdRange { fromID, toID };
    d->request = std::move(request);
}

void MAMTask::onGo()
{
    // Task is one-shot. Do not restart it after Task::done() has fired.
    if (d->progress.started)
        return;
    // All mutable execution state is initialized here, not in get().
    d->progress = Private::Progress {};
    auto &progress = d->progress;
    progress.started = true;
    const auto &request = d->request;
    progress.active = true;

    if (const auto *range = std::get_if<Private::IdRange>(&request.range)) {
        progress.fromId = range->from;
        progress.toId = range->to;
        if (!d->archiveFeatures.test(QStringLiteral("urn:xmpp:mam:2#extended"))) {
            // A compatible one-sided boundary can be translated to baseline
            // RSM without modifying the original request.
            if (!progress.fromId.isEmpty() && progress.toId.isEmpty() && !request.backwards) {
                progress.cursor = progress.fromId;
                progress.visitedCursors.insert(progress.cursor);
                progress.fromId.clear();
            } else if (!progress.toId.isEmpty() && progress.fromId.isEmpty() && request.backwards) {
                progress.cursor = progress.toId;
                progress.visitedCursors.insert(progress.cursor);
                progress.toId.clear();
            } else if (!progress.fromId.isEmpty() || !progress.toId.isEmpty()) {
                setError(1, QStringLiteral("This MAM ID range requires urn:xmpp:mam:2#extended"));
                return;
            }
        }
    }
    getPage();
}

bool MAMTask::take(const QDomElement &x)
{
    if (!d->progress.active)
        return false;

    // The common Task dispatcher offers every stanza to every child task.
    // Each handler must decline stanzas that are not its own.
    if (x.tagName() == QLatin1String("iq"))
        return takeIq(x);
    if (x.tagName() == QLatin1String("message"))
        return takeResult(x);
    return false;
}

bool MAMTask::takeIq(const QDomElement &x)
{
    // A MAM page finishes only on the reply to the IQ we sent. In
    // particular, iqVerify() alone is not enough: for a query of the own
    // archive it can accept replies from another of our account resources.
    if (d->progress.iqId.isEmpty() || x.attribute(QLatin1String("id")) != d->progress.iqId)
        return false;
    const auto type = x.attribute(QLatin1String("type"));
    if (type != QLatin1String("result") && type != QLatin1String("error"))
        return false;
    if (!isExpectedSender(x) || !iqVerify(x, d->archiveJid, d->progress.iqId))
        return false;

    // From this point the IQ is correlated to the pending MAM request.
    // A result without <fin/> is a protocol error, not an unrelated IQ.
    if (x.attribute(QLatin1String("type")) == QLatin1String("error")) {
        if (x.firstChildElement(QLatin1String("error")).isNull())
            setError(1, QStringLiteral("MAM IQ error without error payload"));
        else
            setError(x);
        return true;
    }
    QDomElement fin = directChildNamed(x, QLatin1String("fin"));
    if (fin.isNull() || fin.namespaceURI() != XMPP_MAM_NAMESPACE) {
        setError(1, QStringLiteral("Missing MAM <fin/> in query response"));
        return true;
    }

    d->progress.complete = fin.attribute(QLatin1String("complete")) == QLatin1String("true");
    d->progress.stable = d->progress.stable && fin.attribute(QLatin1String("stable")) != QLatin1String("false");
    if (d->progress.complete || (d->request.maxMessages > 0 && d->progress.fetched >= d->request.maxMessages)) {
        setSuccess();
        return true;
    }

    // XEP-0313 requires <fin/> to contain the RSM first/last IDs,
    // even when the server returned fewer items than we requested.
    const auto set = directChildNamed(fin, QLatin1String("set"));
    if (set.isNull() || set.namespaceURI() != QLatin1String("http://jabber.org/protocol/rsm")) {
        setError(1, QStringLiteral("Missing RSM cursor in MAM <fin/>"));
        return true;
    }
    const auto edge = directChildNamed(set, d->request.backwards ? QLatin1String("first") : QLatin1String("last"));
    const QString cursor = edge.text();
    if (cursor.isEmpty() || d->progress.visitedCursors.contains(cursor)) {
        setError(1, QStringLiteral("MAM pagination returned an empty or repeated cursor"));
        return true;
    }
    d->progress.cursor = cursor;
    d->progress.visitedCursors.insert(cursor);
    getPage();
    return true;
}

bool MAMTask::takeResult(const QDomElement &x)
{
    // Only messages belonging to the current query are archived.
    const QDomElement result = directChildNamed(x, QLatin1String("result"));
    if (result.isNull() || result.namespaceURI() != XMPP_MAM_NAMESPACE
        || result.attribute(QLatin1String("queryid")) != d->progress.queryId)
        return false;

    // A matching queryid is not proof that the sender owns this archive.
    if (!isExpectedSender(x))
        return true; // discard forged result
    const QDomElement forwarded = directChildNamed(result, QLatin1String("forwarded"));
    if (forwarded.isNull() || forwarded.namespaceURI() != QLatin1String("urn:xmpp:forward:0")
        || result.attribute(QLatin1String("id")).isEmpty())
        return true;
    // Validate the envelope before counting or emitting an archive entry.
    // Full message parsing remains the responsibility of the existing
    // Forwarding/Message implementation, not a second parser in MAMTask.
    const auto delay = directChildNamed(forwarded, QLatin1String("delay"));
    const auto message = directChildNamed(forwarded, QLatin1String("message"));
    // XEP-0313 permits tombstones without an inner message. Both the
    // forwarded message and its delay are SHOULD, not MUST. Check either
    // element when present while retaining the archive UID in all cases.
    if ((!delay.isNull()
         && (delay.namespaceURI() != QLatin1String("urn:xmpp:delay")
             || !QDateTime::fromString(delay.attribute(QLatin1String("stamp")), Qt::ISODate).isValid()))
        || (!message.isNull() && message.namespaceURI() != QLatin1String("jabber:client")))
        return true;

    if (d->request.maxMessages > 0 && d->progress.fetched >= d->request.maxMessages) {
        // A server that sends more items than requested must not grow
        // the archive beyond the caller's explicit total limit.
        return true;
    }
    ++d->progress.fetched;
    if (d->collectResults)
        d->progress.archive.append(result);
    emit resultReceived(result);
    return true;
}
