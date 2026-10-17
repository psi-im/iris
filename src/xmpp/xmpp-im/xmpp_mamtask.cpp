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

#include <functional>
#include <utility>
#include <variant>

using namespace XMLHelper;
using namespace XMPP;

namespace {

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

struct TimeRange {
    QDateTime from;
    QDateTime to;
};

struct IdRange {
    QString from;
    QString to;
};

// Configuration belongs to the coordinator. It never changes after go().
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

// Complete, immutable snapshot for exactly one MAM IQ and its streamed results.
// The coordinator supplies effective paging cursors but never owns IQ routing.
struct PageRequest {
    Request request;
    Jid archiveJid;
    Features archiveFeatures;
    QString cursor;     // null means initial page; empty but non-null is invalid
    QString fromId;     // effective #extended filter after baseline RSM fallback
    QString toId;
    int maxResults = 10;
};

// One child Task per MAM page. It is the *only* task that recognizes its
// queryid and IQ id. MAMTask receives completion and validated results only.
class MAMPageTask final : public Task {
public:
    MAMPageTask(Task *parent, PageRequest request, std::function<void(const QDomElement &)> onResult) :
        Task(parent), request_(std::move(request)), onResult_(std::move(onResult))
    {
        // finished() can be emitted during take(). A completed page must
        // immediately stop consuming stanzas, before deleteLater() runs.
        connect(this, &Task::finished, this, [this] { active_ = false; });
    }

    bool isComplete() const { return complete_; }
    bool isStable() const { return stable_; }
    const QString &nextCursor() const { return nextCursor_; }
    void deactivate() { active_ = false; }

    bool take(const QDomElement &stanza) override
    {
        if (!active_)
            return false;
        if (stanza.tagName() == QLatin1String("message"))
            return takeResult(stanza);
        // iqVerify() rejects everything except IQ stanzas matching this
        // page's response ID and expected addressing.
        return takeIq(stanza);
    }

protected:
    void onGo() override
    {
        active_ = true;
        queryId_ = genUniqueID();
        auto iq = createIQ(doc(), QLatin1String("set"), request_.archiveJid.full(), id());
        auto query = doc()->createElementNS(XMPP_MAM_NAMESPACE, QLatin1String("query"));
        query.setAttribute(QLatin1String("queryid"), queryId_);

        SubsetsClientManager rsm;
        rsm.setMax(request_.maxResults);
        if (request_.cursor.isNull()) {
            if (request_.request.backwards)
                rsm.getLast();
            else
                rsm.getFirst();
        } else if (request_.request.backwards) {
            rsm.setFirstID(request_.cursor);
            rsm.getPrevious();
        } else {
            rsm.setLastID(request_.cursor);
            rsm.getNext();
        }

        query.appendChild(makeFilter().toXml(doc()));
        query.appendChild(rsm.makeQueryElement(doc()));
        if (request_.request.flipPages
            && request_.archiveFeatures.test(QStringLiteral("urn:xmpp:mam:2#extended")))
            query.appendChild(doc()->createElementNS(XMPP_MAM_NAMESPACE, QLatin1String("flip-page")));
        iq.appendChild(query);
        send(iq);
    }

private:
    XData makeFilter() const
    {
        XData::FieldList fields;
        if (!request_.request.with.isEmpty()) {
            XData::Field with;
            with.setType(XData::Field::Field_JidSingle);
            with.setVar(QLatin1String("with"));
            with.setValue(QStringList(request_.request.with.full()));
            fields.append(with);
        }
        if (request_.archiveJid.isEmpty()
            && request_.archiveFeatures.test(QStringLiteral("urn:xmpp:mam:2#groupchat-field"))) {
            XData::Field includeGroupchat;
            includeGroupchat.setType(XData::Field::Field_Boolean);
            includeGroupchat.setVar(QLatin1String("include-groupchat"));
            includeGroupchat.setValue(QStringList(
                QLatin1String(request_.request.allowMUCArchives ? "true" : "false")));
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

        if (const auto *range = std::get_if<TimeRange>(&request_.request.range)) {
            if (range->from.isValid())
                addField(QLatin1String("start"), range->from.toUTC().toString(Qt::ISODateWithMs));
            if (range->to.isValid())
                addField(QLatin1String("end"), range->to.toUTC().toString(Qt::ISODateWithMs));
        } else {
            addField(QLatin1String("after-id"), request_.fromId);
            addField(QLatin1String("before-id"), request_.toId);
        }

        XData data;
        data.setType(XData::Data_Submit);
        data.setFields(fields);
        data.setRegistrarType(XMPP_MAM_NAMESPACE);
        return data;
    }

    bool isExpectedSender(const QDomElement &stanza) const
    {
        const Jid sender(stanza.attribute(QLatin1String("from")));
        if (!request_.archiveJid.isEmpty())
            return !sender.isEmpty() && sender.compare(request_.archiveJid, true);
        return sender.isEmpty() || sender.compare(Jid(client()->jid().bare()), true);
    }

    bool takeIq(const QDomElement &iq)
    {
        // iqVerify() already checks the IQ element, response ID and
        // generic JID addressing. It does not check the IQ response type or
        // MAM's stricter sender identity (especially our other resources).
        if (!iqVerify(iq, request_.archiveJid, id()) || !isExpectedSender(iq))
            return false;
        const auto type = iq.attribute(QLatin1String("type"));
        if (type != QLatin1String("result") && type != QLatin1String("error"))
            return false;

        if (type == QLatin1String("error")) {
            if (iq.firstChildElement(QLatin1String("error")).isNull())
                setError(1, QStringLiteral("MAM IQ error without error payload"));
            else
                setError(iq);
            return true;
        }

        const auto fin = directChildNamed(iq, QLatin1String("fin"));
        if (fin.isNull() || fin.namespaceURI() != XMPP_MAM_NAMESPACE) {
            setError(1, QStringLiteral("Missing MAM <fin/> in query response"));
            return true;
        }
        complete_ = fin.attribute(QLatin1String("complete")) == QLatin1String("true");
        stable_ = fin.attribute(QLatin1String("stable")) != QLatin1String("false");

        // Missing/malformed RSM is only an error when another page is
        // required. The coordinator owns that decision (including the
        // global maxMessages cap); this task exposes the cursor, if present.
        const auto rsm = directChildNamed(fin, QLatin1String("set"));
        if (!rsm.isNull() && rsm.namespaceURI() == QLatin1String("http://jabber.org/protocol/rsm")) {
            const auto edge = directChildNamed(rsm,
                request_.request.backwards ? QLatin1String("first") : QLatin1String("last"));
            if (!edge.isNull() && edge.namespaceURI() == QLatin1String("http://jabber.org/protocol/rsm"))
                nextCursor_ = edge.text();
        }
        setSuccess();
        return true;
    }

    bool takeResult(const QDomElement &stanza)
    {
        const auto result = directChildNamed(stanza, QLatin1String("result"));
        if (result.isNull() || result.namespaceURI() != XMPP_MAM_NAMESPACE
            || result.attribute(QLatin1String("queryid")) != queryId_)
            return false;
        // Consume a forged MAM envelope so that it is not surfaced as an
        // ordinary message. Only validated results reach the coordinator.
        if (!isExpectedSender(stanza))
            return true;

        const auto forwarded = directChildNamed(result, QLatin1String("forwarded"));
        if (forwarded.isNull() || forwarded.namespaceURI() != QLatin1String("urn:xmpp:forward:0")
            || result.attribute(QLatin1String("id")).isEmpty())
            return true;
        const auto delay = directChildNamed(forwarded, QLatin1String("delay"));
        const auto message = directChildNamed(forwarded, QLatin1String("message"));
        // Both inner message and delay are optional for MAM tombstones.
        if ((!delay.isNull()
             && (delay.namespaceURI() != QLatin1String("urn:xmpp:delay")
                 || !QDateTime::fromString(delay.attribute(QLatin1String("stamp")), Qt::ISODate).isValid()))
            || (!message.isNull() && message.namespaceURI() != QLatin1String("jabber:client")))
            return true;

        onResult_(result);
        return true;
    }

    PageRequest request_;
    std::function<void(const QDomElement &)> onResult_;
    QString queryId_;
    QString nextCursor_;
    bool active_ = false;
    bool complete_ = false;
    bool stable_ = true;
};

} // namespace

class MAMTask::Private {
public:
    struct Progress {
        int fetched = 0;
        bool complete = false;
        bool stable = true;
        bool started = false;
        bool active = false;
        QString cursor;
        QString fromId;
        QString toId;
        QSet<QString> visitedCursors;
        QList<QDomElement> archive;
        MAMPageTask *currentPage = nullptr; // QObject child, not separately owned
    };

    Request request;
    Progress progress;
    Jid archiveJid;
    Features archiveFeatures;
    bool collectResults = true;
};

MAMTask::MAMTask(Task *parent) : Task(parent), d(std::make_unique<Private>())
{
    // A multi-page query may outlive the default 120-second Task deadline.
    // Each child page keeps its own timeout; callers can still set an
    // explicit overall deadline on this coordinator if needed.
    setTimeout(0);
    connect(this, &Task::finished, this, [this] {
        d->progress.active = false;
        if (d->progress.currentPage) {
            d->progress.currentPage->deactivate();
            d->progress.currentPage->safeDelete();
            d->progress.currentPage = nullptr;
        }
    });
}

MAMTask::~MAMTask() = default;

const QList<QDomElement> &MAMTask::archive() const { return d->progress.archive; }

void MAMTask::setCollectResults(bool enabled)
{
    if (!d->progress.started)
        d->collectResults = enabled;
}

void MAMTask::setArchiveJid(const Jid &jid)
{
    if (!d->progress.started)
        d->archiveJid = jid;
}

void MAMTask::setArchiveFeatures(const Features &features)
{
    if (!d->progress.started)
        d->archiveFeatures = features;
}

bool MAMTask::isComplete() const { return d->progress.complete; }
bool MAMTask::isStable() const { return d->progress.stable; }

void MAMTask::get(const Jid &with, const QDateTime &from, const QDateTime &to,
                  const bool allowMUCArchives, int mamPageSize, int mamMaxMessages,
                  bool flipPages, bool backwards)
{
    if (d->progress.started)
        return;
    auto request = Request::create(with, allowMUCArchives, mamPageSize,
                                   mamMaxMessages, flipPages, backwards);
    request.range = TimeRange { from, to };
    d->request = std::move(request);
}

void MAMTask::get(const Jid &with, const QString &fromID, const QString &toID,
                  const bool allowMUCArchives, int mamPageSize, int mamMaxMessages,
                  bool flipPages, bool backwards)
{
    if (d->progress.started)
        return;
    auto request = Request::create(with, allowMUCArchives, mamPageSize,
                                   mamMaxMessages, flipPages, backwards);
    request.range = IdRange { fromID, toID };
    d->request = std::move(request);
}

void MAMTask::onGo()
{
    if (d->progress.started)
        return; // Base Task is one-shot; a finished task cannot be restarted.
    d->progress = Private::Progress {};
    auto &progress = d->progress;
    progress.started = true;
    progress.active = true;

    if (const auto *ids = std::get_if<IdRange>(&d->request.range)) {
        progress.fromId = ids->from;
        progress.toId = ids->to;
        if (!d->archiveFeatures.test(QStringLiteral("urn:xmpp:mam:2#extended"))) {
            // Translate compatible ID boundaries to baseline RSM without
            // modifying the original request configuration.
            if (!progress.fromId.isEmpty() && progress.toId.isEmpty() && !d->request.backwards) {
                progress.cursor = progress.fromId;
                progress.visitedCursors.insert(progress.cursor);
                progress.fromId.clear();
            } else if (!progress.toId.isEmpty() && progress.fromId.isEmpty() && d->request.backwards) {
                progress.cursor = progress.toId;
                progress.visitedCursors.insert(progress.cursor);
                progress.toId.clear();
            } else if (!progress.fromId.isEmpty() || !progress.toId.isEmpty()) {
                setError(1, QStringLiteral("This MAM ID range requires urn:xmpp:mam:2#extended"));
                return;
            }
        }
    }

    startPage();
}

void MAMTask::startPage()
{
    auto &progress = d->progress;
    const auto &request = d->request;
    if (!progress.active || progress.currentPage)
        return;

    PageRequest page;
    page.request = request;
    page.archiveJid = d->archiveJid;
    page.archiveFeatures = d->archiveFeatures;
    page.cursor = progress.cursor;
    page.fromId = progress.fromId;
    page.toId = progress.toId;
    page.maxResults = request.maxMessages > 0
        ? qMin(request.pageSize, request.maxMessages - progress.fetched)
        : request.pageSize;

    auto *child = new MAMPageTask(this, std::move(page), [this](const QDomElement &result) {
        auto &current = d->progress;
        if (!current.active || (d->request.maxMessages > 0 && current.fetched >= d->request.maxMessages))
            return;
        ++current.fetched;
        if (d->collectResults)
            current.archive.append(result);
        emit resultReceived(result);
    });
    progress.currentPage = child;

    // Each page owns its IQ and MAM result correlation. We only handle
    // completion, update cross-page state and start the next child.
    connect(child, &Task::finished, this, [this, child] {
        auto &current = d->progress;
        if (!current.active || current.currentPage != child) {
            child->safeDelete();
            return;
        }

        const bool ok = child->success();
        const int errorCode = child->statusCode();
        const QString errorText = child->statusString();
        const bool complete = child->isComplete();
        const bool stable = child->isStable();
        const QString cursor = child->nextCursor();

        current.currentPage = nullptr;
        child->safeDelete();
        if (!ok) {
            setError(errorCode, errorText.isEmpty() ? QStringLiteral("MAM page request failed") : errorText);
            return;
        }

        current.complete = complete;
        current.stable = current.stable && stable;
        if (current.complete || (d->request.maxMessages > 0 && current.fetched >= d->request.maxMessages)) {
            setSuccess();
            return;
        }

        if (cursor.isEmpty() || current.visitedCursors.contains(cursor)) {
            setError(1, QStringLiteral("MAM pagination returned an empty or repeated cursor"));
            return;
        }
        current.cursor = cursor;
        current.visitedCursors.insert(cursor);
        startPage();
    });
    child->go();
}

bool MAMTask::take(const QDomElement &stanza)
{
    // The inherited Task dispatcher delegates directly to child page tasks.
    // This coordinator never inspects an IQ or its queryid.
    return d->progress.active && Task::take(stanza);
}
