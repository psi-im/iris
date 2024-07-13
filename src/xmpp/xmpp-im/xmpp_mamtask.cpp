/*
 * xmpp_mam.cpp - XEP-0313 Message Archive Management
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
    int mamPageSize; // TODO: this is the max page size for MAM request. Should be made into a config option
    int       mamMaxMessages; // maximum mam pages total, also should be config
    int                messagesFetched;
    bool flipPages;
    bool backwards;
    bool      allowMUCArchives;
    bool               metadataFetched;
    Jid       j;
    QString firstID;
    QString lastID;
    QString lastArchiveID;
    QDateTime from;
    QDateTime to;
    QList<QDomElement> archive;

    void  getPage(MAMTask* t);
    void  getArchiveMetadata(MAMTask* t);
    XData makeMAMFilter();
};

XData MAMTask::Private::makeMAMFilter() {
    XData::FieldList fl;

    XData::Field with;
    with.setType(XData::Field::Field_JidSingle);
    with.setVar(QLatin1String("with"));
    with.setValue(QStringList(j.bare()));
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

    // TODO: ID filters?

    XData x;
    x.setType(XData::Data_Submit);
    x.setFields(fl);
    x.setRegistrarType(XMPP_MAM_NAMESPACE);

    return x;
}

// TODO: use iris rsm implementation
/*
void MAMTask::Private::getPage(MAMTask* t)
{
    QDomElement iq = createIQ(t->doc(), "set", QString(), t->id());

    QDomElement query = t->doc()->createElementNS(XMPP_MAM_NAMESPACE, "query");

    XData            x = makeMAMFilter();

    QDomElement rsmSet = t->doc()->createElementNS("http://jabber.org/protocol/rsm", "set");
    rsmSet.appendChild(textTag(t->doc(), "max", QString::number(mamMaxMessages)));

    if(flipPages) rsmSet.appendChild(emptyTag(t->doc(), "flip-page"));
    if(backwards) {
        if(lastArchiveID.isNull()) {
            rsmSet.appendChild(emptyTag(t->doc(), "before"));
        } else {
            rsmSet.appendChild(textTag(t->doc(), "before", lastArchiveID));
        }
    } else {
        if(!lastArchiveID.isNull()) {
            rsmSet.appendChild(textTag(t->doc(), "after", lastArchiveID));
        }
    }

    query.appendChild(x.toXml(t->doc()));
    query.appendChild(rsmSet);
    iq.appendChild(query);
    t->send(iq);
}
*/


void MAMTask::Private::getPage(MAMTask* t)
{
    QDomElement iq = createIQ(t->doc(), QLatin1String("set"), QLatin1String(), t->id());
    QDomElement query = t->doc()->createElementNS(XMPP_MAM_NAMESPACE, QLatin1String("query"));
    XData            x = makeMAMFilter();

    SubsetsClientManager rsm;
    rsm.setMax(mamMaxMessages);

    if(flipPages) query.appendChild(emptyTag(t->doc(), QLatin1String("flip-page")));

    if(lastArchiveID.isNull()) {
        if(backwards) {
            rsm.getLast();
        } else {
            rsm.getFirst();
        }
    } else {
        if(backwards) {
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


void MAMTask::Private::getArchiveMetadata(MAMTask* t) {
    // Craft a query to get the first and last messages in an archive
    QDomElement iq = createIQ(t->doc(), QLatin1String("get"), QLatin1String(), t->id());
    QDomElement metadata = emptyTag(t->doc(), QLatin1String("metadata"));
    metadata.setAttribute(QLatin1String("xmlns"), XMPP_MAM_NAMESPACE);
    iq.appendChild(metadata);
    iq.appendChild(makeMAMFilter().toXml(t->doc()));

    t->send(iq);
}

MAMTask::MAMTask(Task *parent) : Task(parent) { d = new Private; }
MAMTask::~MAMTask() { delete d; }

const QList<QDomElement> &MAMTask::archive() const { return d->archive; }

void MAMTask::get(const Jid &j, const QDateTime &from = {}, const QDateTime &to = {},
                  const bool allowMUCArchives = true, int mamPageSize = 10, int mamMaxMessages = 100, bool flipPages = true, bool backwards = true)
{
    d->archive         = {};
    d->messagesFetched = 0;
    d->metadataFetched = false;

    d->j                = j;
    d->from             = from;
    d->to               = to;
    d->allowMUCArchives = allowMUCArchives;
    d->mamPageSize = mamPageSize;
    d->mamMaxMessages   = mamMaxMessages;
    d->flipPages = flipPages;
    d->backwards = backwards;
}

void MAMTask::onGo() {
    d->getArchiveMetadata(this);
}

bool MAMTask::take(const QDomElement &x)
{
    if(d->metadataFetched) {
        if(iqVerify(x, QString(), id())) return false;

        // TODO: only save messages directed at the correct resource?
        d->archive.append(x);
        d->lastArchiveID = x.attribute(QLatin1String("id"));
        d->messagesFetched = d->messagesFetched + 1;

        // Check if we are done
        if (x.attribute(QLatin1String("id")) == d->lastID || d->messagesFetched >= d->mamMaxMessages) {
            setSuccess();
        } else if (d->messagesFetched % d->mamPageSize == 0) { 
            d->getPage(this);
        }
        // TODO: handle server not sending all MAM stuff gracefully (such as handling 0 messagesFetched)
    } else {
        if(!iqVerify(x, QString(), id())) return false;

        if(x.elementsByTagName(QLatin1String("start")).at(0).isNull() || x.elementsByTagName(QLatin1String("end")).at(0).isNull() ) return false;

        if(d->backwards) {
            d->lastID = x.elementsByTagName(QLatin1String("start")).at(0).toElement().attribute(QLatin1String("id"));
            d->firstID = x.elementsByTagName(QLatin1String("end")).at(0).toElement().attribute(QLatin1String("id"));
        } else {
            d->firstID = x.elementsByTagName(QLatin1String("start")).at(0).toElement().attribute(QLatin1String("id"));
            d->lastID = x.elementsByTagName(QLatin1String("end")).at(0).toElement().attribute(QLatin1String("id"));
        }
        d->getPage(this);
        d->metadataFetched = true;
    }

    return true;
}
