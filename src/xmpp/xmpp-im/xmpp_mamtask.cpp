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

using namespace XMLHelper;
using namespace XMPP;

class MAMTask::Private {
public:
    int     mamPageSize; // TODO: this is the max page size for MAM request. Should be made into a config option in Psi+
    int     mamMaxMessages; // maximum mam pages total, also should be config. zero means unlimited
    int     messagesFetched;
    bool    flipPages;
    bool    backwards;
    bool    allowMUCArchives;
    bool    metadataFetched;
    Jid     j;
    QString firstID;
    QString lastID;
    QString lastArchiveID;
    QString from_id;
    QString to_id;
    QDateTime          from;
    QDateTime          to;
    QList<QDomElement> archive;

    void  getPage(MAMTask *t);
    void  getArchiveMetadata(MAMTask *t);
    XData makeMAMFilter();
};


MAMTask::MAMTask(Task *parent) : Task(parent) { d = new Private; }
MAMTask::MAMTask(const MAMTask& x) : Task(x.parent()) { d = x.d; }
MAMTask::~MAMTask() { delete d; }

const QList<QDomElement> &MAMTask::archive() const { return d->archive; }

XData MAMTask::Private::makeMAMFilter()
{
    XData::FieldList fl;

    XData::Field with;
    with.setType(XData::Field::Field_JidSingle);
    with.setVar(QLatin1String("with"));
    with.setValue(QStringList(j.full()));
    fl.append(with);

    XData::Field includeGroupchat;
    includeGroupchat.setType(XData::Field::Field_Boolean);
    includeGroupchat.setVar(QLatin1String("include-groupchat"));
    includeGroupchat.setValue(QStringList(QLatin1String(allowMUCArchives ? "true" : "false")));
    fl.append(includeGroupchat);

    if (from.isValid()) {
        XData::Field start;
        start.setType(XData::Field::Field_TextSingle);
        start.setVar(QLatin1String("start"));
        from.setTimeSpec(Qt::UTC);
        start.setValue(QStringList(from.toString()));
        fl.append(start);
    }

    if (to.isValid()) {
        XData::Field end;
        end.setType(XData::Field::Field_TextSingle);
        end.setVar(QLatin1String("end"));
        to.setTimeSpec(Qt::UTC);
        end.setValue(QStringList(to.toString()));
        fl.append(end);
    }

    if (!from_id.isNull()) {
        XData::Field start_id;
        start_id.setType(XData::Field::Field_TextSingle);
        start_id.setVar(QLatin1String("after-id"));
        start_id.setValue(QStringList(from_id));
        fl.append(start_id);
    }

    if (!to_id.isNull()) {
        XData::Field end_id;
        end_id.setType(XData::Field::Field_TextSingle);
        end_id.setVar(QLatin1String("before-id"));
        end_id.setValue(QStringList(to_id));
        fl.append(end_id);
    }

    XData x;
    x.setType(XData::Data_Submit);
    x.setFields(fl);
    x.setRegistrarType(XMPP_MAM_NAMESPACE);

    return x;
}

void MAMTask::Private::getPage(MAMTask *t)
{
    QDomElement iq    = createIQ(t->doc(), QLatin1String("set"), QLatin1String(), t->id());
    QDomElement query = t->doc()->createElementNS(XMPP_MAM_NAMESPACE, QLatin1String("query"));
    XData       x     = makeMAMFilter();

    SubsetsClientManager rsm;
    rsm.setMax(mamMaxMessages);

    if (flipPages)
        query.appendChild(emptyTag(t->doc(), QLatin1String("flip-page")));

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

    query.appendChild(x.toXml(t->doc()));
    query.appendChild(rsm.makeQueryElement(t->doc()));
    iq.appendChild(query);
    t->send(iq);
}

void MAMTask::Private::getArchiveMetadata(MAMTask *t)
{
    // Craft a query to get the first and last messages in an archive
    QDomElement iq       = createIQ(t->doc(), QLatin1String("get"), QLatin1String(), t->id());
    QDomElement metadata = emptyTag(t->doc(), QLatin1String("metadata"));
    metadata.setAttribute(QLatin1String("xmlns"), XMPP_MAM_NAMESPACE);
    iq.appendChild(metadata);
    iq.appendChild(makeMAMFilter().toXml(t->doc()));

    t->send(iq);
}

// Note: Set `j` to a resource if you just want to query that resource
// if you want to query all resources, set `j` to the bare JID

// Filter by time range
void MAMTask::get(const Jid &j, const QDateTime &from, const QDateTime &to, const bool allowMUCArchives,
                  int mamPageSize, int mamMaxMessages, bool flipPages, bool backwards)
{
    d->archive         = {};
    d->messagesFetched = 0;
    d->metadataFetched = false;

    d->j                = j;
    d->from             = from;
    d->to               = to;
    d->allowMUCArchives = allowMUCArchives;
    d->mamPageSize      = mamPageSize;
    d->mamMaxMessages   = mamMaxMessages;
    d->flipPages        = flipPages;
    d->backwards        = backwards;
}

// Filter by id range
void MAMTask::get(const Jid &j, const QString &from_id, const QString &to_id, const bool allowMUCArchives,
                  int mamPageSize, int mamMaxMessages, bool flipPages, bool backwards)
{
    d->archive         = {};
    d->messagesFetched = 0;
    d->metadataFetched = false;

    d->j                = j;
    d->from_id          = from_id;
    d->to_id            = to_id;
    d->allowMUCArchives = allowMUCArchives;
    d->mamPageSize      = mamPageSize;
    d->mamMaxMessages   = mamMaxMessages;
    d->flipPages        = flipPages;
    d->backwards        = backwards;
}

void MAMTask::onGo() { d->getArchiveMetadata(this); }

bool MAMTask::take(const QDomElement &x)
{
    if (d->metadataFetched) {
        if (iqVerify(x, QString(), id())) {
            if (!x.elementsByTagNameNS(QLatin1String("urn:ietf:params:xml:ns:xmpp-stanzas"),
                                       QLatin1String("item-not-found"))
                     .isEmpty()) {
                setError(2, "First or last stanza UID of filter was not found in the archive");
                return true;
            } else if (!x.elementsByTagNameNS(XMPP_MAM_NAMESPACE, QLatin1String("fin")).isEmpty()) {
                // We are done?
                setSuccess();
                return true;
            }
            // Probably ignore it
            return false;
        }

        d->archive.append(x);
        d->lastArchiveID   = x.attribute(QLatin1String("id"));
        d->messagesFetched = d->messagesFetched + 1;

        // Check if we are done
        if (x.attribute(QLatin1String("id")) == d->lastID || d->messagesFetched >= d->mamMaxMessages) {
            setSuccess();
        } else if (d->messagesFetched % d->mamPageSize == 0) {
            d->getPage(this);
        }
    } else {
        if (!iqVerify(x, QString(), id()) || x.elementsByTagName(QLatin1String("metadata")).isEmpty())
            return false;

        // Return if the archive is empty
        if (!x.elementsByTagName(QLatin1String("metadata")).at(0).hasChildNodes()) {
            setError(1, QLatin1String("Archive is empty"));
            return true;
        }

        if (x.elementsByTagName(QLatin1String("start")).at(0).isNull()
            || x.elementsByTagName(QLatin1String("end")).at(0).isNull())
            return false;

        if (d->backwards) {
            d->lastID  = x.elementsByTagName(QLatin1String("start")).at(0).toElement().attribute(QLatin1String("id"));
            d->firstID = x.elementsByTagName(QLatin1String("end")).at(0).toElement().attribute(QLatin1String("id"));
        } else {
            d->firstID = x.elementsByTagName(QLatin1String("start")).at(0).toElement().attribute(QLatin1String("id"));
            d->lastID  = x.elementsByTagName(QLatin1String("end")).at(0).toElement().attribute(QLatin1String("id"));
        }
        d->getPage(this);
        d->metadataFetched = true;
    }

    return true;
}
