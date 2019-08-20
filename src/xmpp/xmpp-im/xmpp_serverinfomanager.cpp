/*
 * xmpp_serverinfomanager.cpp
 * Copyright (C) 2006-2019  Remko Troncon, Sergey Ilinykh
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "xmpp_serverinfomanager.h"

#include "xmpp_caps.h"
#include "xmpp_tasks.h"

namespace XMPP {
ServerInfoManager::ServerInfoManager(Client* client):
    QObject(client),
    _client(client),
    _canMessageCarbons(false)
{
    deinitialize();
    // NOTE we can use this class for any server, but for this we shouldn't use roster signal here
    connect(_client, SIGNAL(rosterRequestFinished(bool, int, const QString &)), SLOT(initialize()), Qt::QueuedConnection);
}

void ServerInfoManager::reset()
{
    _hasPEP = false;
    _multicastService.clear();
    _extraServerInfo.clear();
    disconnect(CapsRegistry::instance());
    disconnect(_client, SIGNAL(disconnected()), this, SLOT(deinitialize()));
}

void ServerInfoManager::initialize()
{
    connect(_client, SIGNAL(disconnected()), SLOT(deinitialize()));
    JT_DiscoInfo *jt = new JT_DiscoInfo(_client->rootTask());
    connect(jt, SIGNAL(finished()), SLOT(disco_finished()));
    jt->get(_client->jid().domain());
    jt->go(true);

    queryServicesList();
}

void ServerInfoManager::deinitialize()
{
    reset();
    emit featuresChanged();
}

const QString& ServerInfoManager::multicastService() const
{
    return _multicastService;
}

bool ServerInfoManager::hasPEP() const
{
    return _hasPEP;
}

bool ServerInfoManager::canMessageCarbons() const
{
    return _canMessageCarbons;
}

void ServerInfoManager::queryServicesList()
{
    _servicesListState = ST_InProgress;
    auto jtitems = new JT_DiscoItems(_client->rootTask());
    connect(jtitems, &JT_DiscoItems::finished, this, [=]()
    {
        _servicesInfo.clear(); //
        if (jtitems->success()) {
            _servicesListState = ST_Ready;
            for (const auto &item: jtitems->items()) {
                _servicesInfo.insert(item.jid().full(), {ST_NotQueried, item, QMap<QString,QVariant>()});
            }
        }
        else {
            _servicesListState = ST_Failed;
        }
        checkPendingServiceQueries();
    }, Qt::QueuedConnection);
    jtitems->get(_client->jid().domain());
    jtitems->go(true);
}

void ServerInfoManager::checkPendingServiceQueries()
{
    // if services list is not ready yet we have to exit. if it's failed we have to finish all pending queries
    if (_servicesListState != ST_Ready) {
        if (_servicesListState == ST_Failed) {
            const auto sqs = _serviceQueries;
            _serviceQueries.clear();
            for (const auto &q: sqs) {
                q.callback(QList<DiscoItem>());
            }
        }
        return;
    }

    // services list is ready here and we can start checking it and sending disco#info to not cached entries
    auto sqIt = _serviceQueries.begin();
    while(sqIt != _serviceQueries.end()) {

        // populate services to query for this service request
        if (!sqIt->servicesToQueryDefined) {
            sqIt->spareServicesToQuery.clear();
            // grep all suitble service jids. moving forward preferred ones
            QMapIterator<QString,ServiceInfo> si(_servicesInfo);
            while (si.hasNext()) {
                si.next();
                if (!sqIt->nameHint.isEmpty()) {
                    if (sqIt->nameHint.isEmpty() || sqIt->nameHint.exactMatch(si.key())) {
                        sqIt->servicesToQuery.append(si.key());
                    } else if (sqIt->options & SQ_CheckAllOnNoMatch) {
                        sqIt->spareServicesToQuery.append(si.key());
                    }
                } else {
                    sqIt->servicesToQuery.append(si.key());
                }
            }
            if (sqIt->servicesToQuery.isEmpty()) {
                sqIt->servicesToQuery = sqIt->spareServicesToQuery;
                sqIt->spareServicesToQuery.clear();
            }
            if (sqIt->servicesToQuery.isEmpty()) {
                sqIt->callback(QList<DiscoItem>());
                _serviceQueries.erase(sqIt++);
                continue;
            }
            sqIt->servicesToQueryDefined = true;
        }

        // now `sqIt->servicesToQuery` definitely has something to check. maybe some info is already in cache.
        bool hasInProgress = false;
        auto jidIt = sqIt->servicesToQuery.begin();
        //bool foundMatch = false;
        while (jidIt != sqIt->servicesToQuery.end()) {
            auto si = _servicesInfo.find(*jidIt); // find cached service corresponding to one of matched jids
            if (si == _servicesInfo.end() || si->state == ST_Failed) {
                // the map was updated after the first service list request, or info request failed.
                sqIt->servicesToQuery.erase(jidIt++);
                continue;
            } else
            if (si->state == ST_Ready) { // disco info finished successfully for current jid from `servicesToQuery`
                bool foundIdentity = sqIt->category.isEmpty() && sqIt->type.isEmpty();
                if (!foundIdentity) {
                    for (auto &i: si->item.identities()) {
                        if ((sqIt->category.isEmpty() || sqIt->category == i.category) &&
                                (sqIt->type.isEmpty() || sqIt->type == i.type))
                        {
                            foundIdentity = true;
                            break;
                        }
                    }
                }
                if (foundIdentity && (sqIt->features.isEmpty() || std::accumulate(sqIt->features.constBegin(),
                                                                  sqIt->features.constEnd(), false,
                                                                  [&si](bool a, const QSet<QString> &b) {
                                                                     return a || si->item.features().test(b);
                                                                  })))
                {
                    sqIt->result.append(si->item);
                    if (sqIt->options & SQ_FinishOnFirstMatch) {
                        break;
                    }
                }
                sqIt->servicesToQuery.erase(jidIt++);
                continue;
            }

            // if we a here then service info state is either not-queried or in-progress
            Q_ASSERT(si->state == ST_NotQueried || si->state == ST_InProgress);
            hasInProgress = true;
            if (si->state == ST_NotQueried) { // if not queried then let's query
                si->state = ST_InProgress;
                auto jtinfo = new JT_DiscoInfo(_client->rootTask());
                connect(jtinfo, &DiscoInfoTask::finished, this, [this, jtinfo](){
                    auto si = _servicesInfo.find(jtinfo->jid().full());
                    if (si != _servicesInfo.end()) {
                        if (jtinfo->success()) {
                            si.value().state = ST_Ready;
                            si.value().item = jtinfo->item();
                        } else {
                            si.value().state = ST_Failed;
                        }
                    }
                    checkPendingServiceQueries();
                });
                jtinfo->get(Jid(*jidIt), si.value().item.node());
                jtinfo->go(true);
            }
            ++jidIt;
        }

        if (sqIt->result.isEmpty() && !hasInProgress && !sqIt->spareServicesToQuery.isEmpty()) {
            // we don't check sqIt->servicesToQuery.isEmpty() since it comes from other conditions (sqIt->result.isEmpty() && !hasInProgress)
            sqIt->servicesToQuery = sqIt->spareServicesToQuery;
            sqIt->spareServicesToQuery.clear();
            continue; // continue with the same ServiceQuery but with different jids list
        }

        // if has at least one sufficient result
        auto forceFinish = (!sqIt->result.isEmpty() && (sqIt->options & SQ_FinishOnFirstMatch)); // stop on first found
        // if nothing in progress then we have full result set or nothing found even in spare list
        if (forceFinish || !hasInProgress) { // self explanatory
            auto callback = std::move(sqIt->callback);
            auto result = sqIt->result;
            _serviceQueries.erase(sqIt++);
            callback(result);
        } else {
            ++sqIt;
        }
    }
}

void ServerInfoManager::appendQuery(const ServiceQuery &q)
{
    _serviceQueries.push_back(q);
    if (_servicesListState == ST_InProgress) {
        return;
    }
    if (_servicesListState == ST_NotQueried || _servicesListState == ST_Failed) {
        queryServicesList();
    } else { // ready
        checkPendingServiceQueries();
    }
}

void ServerInfoManager::queryServiceInfo(const QString &category, const QString &type, const QList<QSet<QString>> &features,
                                     const QRegExp &nameHint, SQOptions options, std::function<void (const QList<DiscoItem> &items)> callback)
{
    appendQuery(ServiceQuery(type, category, features, nameHint, options, std::move(callback)));
}

void ServerInfoManager::setServiceMeta(const Jid &service, const QString &key, const QVariant &value)
{
    auto it = _servicesInfo.find(service.full());
    if (it != _servicesInfo.end()) {
        it.value().meta.insert(key, value);
    }
}

QVariant ServerInfoManager::serviceMeta(const Jid &service, const QString &key)
{
    auto it = _servicesInfo.find(service.full());
    if (it != _servicesInfo.end()) {
        return it.value().meta.value(key);
    }
    return QVariant();
}

void ServerInfoManager::disco_finished()
{
    JT_DiscoInfo *jt = static_cast<JT_DiscoInfo *>(sender());
    if (jt->success()) {
        _features = jt->item().features();

        if (_features.hasMulticast())
            _multicastService = _client->jid().domain();

        _canMessageCarbons = _features.hasMessageCarbons();

        // Identities
        DiscoItem::Identities is = jt->item().identities();
        foreach(DiscoItem::Identity i, is) {
            if (i.category == "pubsub" && i.type == "pep")
                _hasPEP = true;
        }

        for (const auto &x: jt->item().extensions()) {
            if (x.type() == XData::Data_Result && x.registrarType() == QLatin1String("http://jabber.org/network/serverinfo")) {
                for (const auto &f: x.fields()) {
                    if (f.type() == XData::Field::Field_ListMulti) {
                        _extraServerInfo.insert(f.var(), f.value()); // covers XEP-0157
                    }
                }
            }
        }

        emit featuresChanged();
    }
}
} // namespace XMPP
