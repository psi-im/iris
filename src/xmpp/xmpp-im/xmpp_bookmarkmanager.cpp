/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "xmpp_bookmarkmanager.h"

#include "xmpp_client.h"
#include "xmpp_features.h"
#include "xmpp_serverinfomanager.h"
#include "xmpp_xmlcommon.h"

#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <optional>

namespace XMPP {
namespace {
const QString legacyNs = QStringLiteral("storage:bookmarks");

bool same(const NativeBookmark &a, const NativeBookmark &b)
{
    return a.room().bare() == b.room().bare() && a.name() == b.name()
        && a.nick() == b.nick() && a.password() == b.password() && a.autoJoin() == b.autoJoin();
}

bool same(const QList<NativeBookmark> &a, const QList<NativeBookmark> &b)
{
    if (a.size() != b.size())
        return false;
    QHash<QString, NativeBookmark> indexed;
    for (const auto &item : a)
        indexed.insert(item.room().bare(), item);
    for (const auto &item : b) {
        auto it = indexed.constFind(item.room().bare());
        if (it == indexed.cend() || !same(it.value(), item))
            return false;
    }
    return true;
}

QList<NativeBookmark> decodeConferences(const QDomElement &storage)
{
    QList<NativeBookmark> rooms;
    for (auto el = storage.firstChildElement(); !el.isNull(); el = el.nextSiblingElement()) {
        if (el.tagName() != QLatin1String("conference"))
            continue;
        const Jid room(el.attribute(QStringLiteral("jid")));
        if (!room.isValid() || room.isEmpty() || !room.resource().isEmpty())
            continue;
        const auto join = el.attribute(QStringLiteral("autojoin"));
        rooms.append(NativeBookmark::create(room, el.attribute(QStringLiteral("name")),
                   join == QLatin1String("true") || join == QLatin1String("1"),
                   el.firstChildElement(QStringLiteral("nick")).text(),
                   el.firstChildElement(QStringLiteral("password")).text()));
    }
    return rooms;
}

QList<BookmarkUrl> decodeUrls(const QDomElement &storage)
{
    QList<BookmarkUrl> urls;
    for (auto el = storage.firstChildElement(); !el.isNull(); el = el.nextSiblingElement())
        if (el.tagName() == QLatin1String("url"))
            urls.append({ el.attribute(QStringLiteral("name")), el.attribute(QStringLiteral("url")) });
    return urls;
}

class PrivateBookmarksTask final : public Task {
public:
    explicit PrivateBookmarksTask(Task *parent) : Task(parent) { }

    void get() { setup(QStringLiteral("get")); }
    void set(const QList<BookmarkUrl> &urls, const QList<NativeBookmark> &conferences)
    {
        setup(QStringLiteral("set"));
        urls_ = urls;
        conferences_ = conferences;
        auto storage = iq_.firstChildElement(QStringLiteral("query")).firstChildElement(QStringLiteral("storage"));
        for (const auto &url : urls) {
            auto item = doc()->createElement(QStringLiteral("url"));
            item.setAttribute(QStringLiteral("name"), url.name);
            item.setAttribute(QStringLiteral("url"), url.url);
            storage.appendChild(item);
        }
        for (const auto &room : conferences) {
            if (!room.isValid())
                continue;
            auto el = doc()->createElement(QStringLiteral("conference"));
            el.setAttribute(QStringLiteral("jid"), room.room().bare());
            el.setAttribute(QStringLiteral("name"), room.name());
            el.setAttribute(QStringLiteral("autojoin"), room.autoJoin() ? QStringLiteral("true") : QStringLiteral("false"));
            const auto child = [this, &el](const QString &tag, const QString &text) {
                if (text.isEmpty())
                    return;
                auto node = doc()->createElement(tag);
                node.appendChild(doc()->createTextNode(text));
                el.appendChild(node);
            };
            child(QStringLiteral("nick"), room.nick());
            child(QStringLiteral("password"), room.password());
            storage.appendChild(el);
        }
    }
    const QList<BookmarkUrl> &urls() const { return urls_; }
    const QList<NativeBookmark> &conferences() const { return conferences_; }

protected:
    void onGo() override { send(iq_); }
    bool take(const QDomElement &stanza) override
    {
        if (!iqVerify(stanza, Jid(), id()))
            return false;
        if (stanza.attribute(QStringLiteral("type")) != QLatin1String("result")) {
            setError(stanza);
            return true;
        }
        if (get_) {
            const auto query = stanza.firstChildElement(QStringLiteral("query"));
            if (!query.isNull() && query.namespaceURI() == QLatin1String("jabber:iq:private")) {
                auto storage = query.firstChildElement(QStringLiteral("storage"));
                if (!storage.isNull() && storage.namespaceURI() == legacyNs) {
                    urls_ = decodeUrls(storage);
                    conferences_ = decodeConferences(storage);
                }
            }
        }
        setSuccess();
        return true;
    }

private:
    void setup(const QString &type)
    {
        get_ = type == QLatin1String("get");
        iq_ = createIQ(doc(), type, QString(), id());
        auto query = doc()->createElementNS(QStringLiteral("jabber:iq:private"), QStringLiteral("query"));
        iq_.appendChild(query);
        query.appendChild(doc()->createElementNS(legacyNs, QStringLiteral("storage")));
    }
    QDomElement iq_;
    bool get_ = false;
    QList<BookmarkUrl> urls_;
    QList<NativeBookmark> conferences_;
};
} // namespace

class BookmarksManager::Private {
public:
    Client *client = nullptr;
    bool online = false;
    bool available = false;
    bool nativeActive = false;
    bool nativeWasActive = false;
    bool batchingEdits = false;
    bool legacyPending = false;
    Jid accountJid;
    bool legacyWriting = false;
    bool pendingUrlsValid = false;
    bool legacyQueued = false;
    QList<NativeBookmark> queuedLegacyConferences;
    QList<BookmarkUrl> queuedLegacyUrls;
    int retryDelayMs = 0;
    quint64 generation = 1;
    PubSubReplica<NativeBookmark> replica;
    QList<NativeBookmark> conferences;
    QList<BookmarkUrl> urls;
    QList<BookmarkUrl> pendingUrls;
};

BookmarksManager::BookmarksManager(Client *client, QObject *parent)
    : QObject(parent ? parent : client), d(std::make_unique<Private>())
{
    d->client = client;
    auto *pubsub = client->pubSubManager();
    connect(client->serverInfoManager(), &ServerInfoManager::featuresChanged,
            this, &BookmarksManager::maybeEnableNative);
    connect(pubsub, &PubSubManager::itemPublished, this,
            [this](const Jid &service, const QString &node, const PubSubItem &item) {
        if (!d->online || (service.bare() != d->client->jid().bare() || !service.resource().isEmpty()) || node != NativeBookmark::node())
            return;
        const auto native = NativeBookmark::fromItem(item);
        if (!native.isValid())
            return;
        d->replica.published(native.room().bare(), native);
        if (d->nativeActive)
            rebuildNativeConferences(ChangeSource::Push);
    });
    connect(pubsub, &PubSubManager::itemRetracted, this,
            [this](const Jid &service, const QString &node, const QString &id) {
        if (!d->online || (service.bare() != d->client->jid().bare() || !service.resource().isEmpty()) || node != NativeBookmark::node())
            return;
        d->replica.retracted(id);
        if (d->nativeActive)
            rebuildNativeConferences(ChangeSource::Push);
    });
    const auto invalidate = [this](const Jid &service, const QString &node) {
        if (!d->online || (service.bare() != d->client->jid().bare() || !service.resource().isEmpty()) || node != NativeBookmark::node()
            || (!d->nativeActive && d->replica.state() != PubSubReplica<NativeBookmark>::State::Loading))
            return;
        d->replica.invalidate();
        d->nativeActive = false;
        fetchNative();
    };
    connect(pubsub, &PubSubManager::nodePurged, this, invalidate);
    connect(pubsub, &PubSubManager::nodeDeleted, this, invalidate);
    connect(client, &Client::disconnected, this, [this] { setOnline(false); });
}

BookmarksManager::~BookmarksManager() = default;
bool BookmarksManager::isAvailable() const { return d->available; }
bool BookmarksManager::usesNativeBookmarks() const { return d->nativeActive; }
const QList<NativeBookmark> &BookmarksManager::conferences() const { return d->conferences; }
const QList<BookmarkUrl> &BookmarksManager::urls() const { return d->urls; }

void BookmarksManager::setAvailable(bool value)
{
    if (d->available == value)
        return;
    d->available = value;
    emit availabilityChanged(value);
}

void BookmarksManager::setOnline(bool online)
{
    const auto jid = d->client->jid().bare();
    const bool changedAccount = online && !d->accountJid.isEmpty() && jid != d->accountJid;
    if (d->online == online && !changedAccount)
        return;
    d->online = online;
    ++d->generation;
    if (changedAccount) {
        // A Client object can be reused with different credentials. Never
        // replay the previous identity's pending data on the new account.
        d->replica.reset();
        d->nativeWasActive = false;
        d->nativeActive = false;
        d->pendingUrlsValid = false;
        d->pendingUrls.clear();
        d->legacyQueued = false;
        d->queuedLegacyUrls.clear();
        d->queuedLegacyConferences.clear();
        d->retryDelayMs = 0;
        if (!d->urls.isEmpty()) {
            d->urls.clear();
            emit urlsChanged();
        }
        if (!d->conferences.isEmpty()) {
            d->conferences.clear();
            emit conferencesChanged();
        }
        setAvailable(false);
    }
    if (online) {
        d->accountJid = jid;
        fetchLegacy();
    } else {
        d->replica.reset(d->nativeWasActive);
        d->nativeActive = false;
        d->legacyPending = false;
        d->legacyWriting = false;
        setAvailable(false);
    }
}

void BookmarksManager::fetchLegacy()
{
    if (!d->online || d->legacyPending)
        return;
    d->legacyPending = true;
    const auto gen = d->generation;
    auto *task = new PrivateBookmarksTask(d->client->rootTask());
    connect(task, &Task::finished, this, [this, task, gen] {
        if (gen != d->generation || !d->online)
            return;
        d->legacyPending = false;
        if (task->success()) {
            if (d->urls != task->urls()) {
                d->urls = task->urls();
                emit urlsChanged();
            }
            if (!d->nativeWasActive && !d->nativeActive && !same(d->conferences, task->conferences())) {
                d->conferences = task->conferences();
                emit conferencesChanged();
            }
            setAvailable(true);
        } else if (!d->nativeWasActive) {
            setAvailable(false);
        }
        maybeEnableNative();
    });
    task->get();
    task->go(true);
}

void BookmarksManager::maybeEnableNative()
{
    if (!d->online || d->legacyPending || d->nativeActive
        || d->replica.state() == PubSubReplica<NativeBookmark>::State::Loading)
        return;
    auto *server = d->client->serverInfoManager();
    const auto &accountFeatures = server->accountFeatures();
    const bool compat = accountFeatures.test("urn:xmpp:bookmarks:1#compat");
    const bool publishOptions = accountFeatures.test("http://jabber.org/protocol/pubsub#publish-options")
        || server->serverFeatures().test("http://jabber.org/protocol/pubsub#publish-options");
    if (compat && server->hasPEP() && publishOptions)
        fetchNative();
}

void BookmarksManager::scheduleNativeRetry()
{
    d->retryDelayMs = d->retryDelayMs ? qMin(d->retryDelayMs * 2, 60000) : 2000;
    const auto gen = d->generation;
    QTimer::singleShot(d->retryDelayMs, this, [this, gen] {
        if (gen == d->generation && d->online && d->nativeWasActive && !d->nativeActive
            && d->replica.state() == PubSubReplica<NativeBookmark>::State::ResyncRequired)
            fetchNative();
    });
}

void BookmarksManager::fetchNative()
{
    const auto token = d->replica.beginLoad();
    if (!token)
        return;
    const auto gen = d->generation;
    auto *task = d->client->pubSubManager()->items(Jid(), NativeBookmark::node());
    connect(task, &Task::finished, this, [this, task, gen, token] {
        if (gen != d->generation || !d->online)
            return;
        const bool noNode = !task->success()
            && task->error().condition == Stanza::Error::ErrorCond::ItemNotFound;
        if (!task->success() && !(d->nativeWasActive && noNode)) {
            if (d->replica.failLoad(token) && d->nativeWasActive)
                scheduleNativeRetry();
            return;
        }
        PubSubReplica<NativeBookmark>::Items snapshot;
        if (task->success())
            for (const auto &item : task->items()) {
                const auto native = NativeBookmark::fromItem(item);
                if (native.isValid())
                    snapshot.insert(native.room().bare(), native);
            }
        if (!d->replica.finishLoad(token, std::move(snapshot)))
            return;
        // Do not silently replace an existing legacy collection by an empty
        // initially-unmigrated PEP node. This is a policy decision, not a merge.
        if (!d->nativeWasActive && d->replica.items().isEmpty() && !d->conferences.isEmpty()) {
            d->replica.invalidate();
            return;
        }
        d->nativeActive = true;
        d->nativeWasActive = true;
        d->retryDelayMs = 0;
        setAvailable(true);
        rebuildNativeConferences();
        flushNativeWrites();
    });
    task->go(true);
}

void BookmarksManager::rebuildNativeConferences(ChangeSource source)
{
    // Never expose the stale last IQ as the UI's authoritative state while
    // a user intent remains queued or in-flight.
    const auto optimistic = d->replica.optimisticItems();
    QStringList ids = optimistic.keys();
    ids.sort(Qt::CaseInsensitive);
    QList<NativeBookmark> next;
    for (const auto &id : ids)
        next.append(optimistic.value(id));
    if (same(d->conferences, next))
        return;

    QHash<QString, NativeBookmark> before, after;
    for (const auto &room : d->conferences)
        before.insert(room.room().bare(), room);
    for (const auto &room : next)
        after.insert(room.room().bare(), room);
    d->conferences = std::move(next);
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (!after.contains(it.key()))
            emit conferenceRemoved(it.value().room(), source);
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        const auto previous = before.constFind(it.key());
        if (previous == before.cend() || !same(previous.value(), it.value()))
            emit conferenceUpdated(it.value(), source);
    }
    emit conferencesChanged();
}

void BookmarksManager::setBookmarks(const QList<BookmarkUrl> &urls,
                                    const QList<NativeBookmark> &conferences)
{
    if (!d->online)
        return;
    if (!d->nativeWasActive && !d->nativeActive) {
        saveLegacy(urls, conferences);
        return;
    }
    setConferences(conferences);
    setUrls(urls);
}

void BookmarksManager::setConferences(const QList<NativeBookmark> &conferences)
{
    if (!d->online)
        return;
    QSet<QString> ids;
    for (const auto &room : conferences) {
        if (!room.isValid() || ids.contains(room.room().bare()))
            return;
        ids.insert(room.room().bare());
    }

    if (!d->nativeWasActive && !d->nativeActive) {
        saveLegacy(d->urls, conferences);
        return;
    }

    for (const auto &room : conferences) {
        const auto id = room.room().bare();
        auto it = std::find_if(d->conferences.cbegin(), d->conferences.cend(),
                              [&id](const NativeBookmark &v) { return v.room().bare() == id; });
        if (it != d->conferences.cend() && same(*it, room)
            && !d->replica.hasScheduledIntent(id))
            continue; // An undo still has to supersede earlier unconfirmed edits.
        auto native = d->replica.items().value(id);
        if (!native.isValid())
            native = NativeBookmark::create(room.room(), room.name(), room.autoJoin(), room.nick(), room.password());
        else {
            native.setName(room.name());
            native.setAutoJoin(room.autoJoin());
            native.setNick(room.nick());
            native.setPassword(room.password());
        }
        d->replica.put(id, native);
    }
    for (const auto &room : d->conferences)
        if (!ids.contains(room.room().bare()))
            d->replica.remove(room.room().bare());
    // Undoing a newly added room before its publish begins should cancel
    // the queued insertion outright. If its publish is already in flight,
    // a later retract is necessary because the server may accept it.
    for (const auto &id : d->replica.scheduledIds()) {
        if (ids.contains(id) || d->replica.items().contains(id))
            continue;
        if (d->replica.hasInFlightIntent(id))
            d->replica.remove(id);
        else
            d->replica.cancelPending(id);
    }
    flushNativeWrites();
    if (!d->nativeActive && d->replica.state() != PubSubReplica<NativeBookmark>::State::Loading)
        fetchNative();
}

void BookmarksManager::applyConferenceEdits(const QList<NativeBookmark> &baseline,
                                             const QList<NativeBookmark> &edited)
{
    QHash<QString, NativeBookmark> before, after;
    for (const auto &room : baseline)
        before.insert(room.room().bare(), room);
    for (const auto &room : edited) {
        if (!room.isValid() || after.contains(room.room().bare()))
            return;
        after.insert(room.room().bare(), room);
    }
    QList<NativeBookmark> merged = d->conferences;
    for (const auto &old : baseline)
        if (!after.contains(old.room().bare()))
            merged.erase(std::remove_if(merged.begin(), merged.end(),
                [&old](const NativeBookmark &b) { return b.room().bare() == old.room().bare(); }), merged.end());
    for (const auto &next : edited) {
        auto old = before.constFind(next.room().bare());
        if (old != before.cend() && same(old.value(), next))
            continue;
        merged.erase(std::remove_if(merged.begin(), merged.end(),
            [&next](const NativeBookmark &b) { return b.room().bare() == next.room().bare(); }), merged.end());
        merged.append(next);
    }
    setConferences(merged);
}

void BookmarksManager::setUrls(const QList<BookmarkUrl> &urls)
{
    if (!d->online)
        return;
    // A second edit can undo a still queued or unacknowledged URL write.
    if (urls == d->urls && !d->pendingUrlsValid && !d->legacyWriting && !d->legacyQueued)
        return;
    if (d->nativeWasActive) {
        d->pendingUrls = urls;
        d->pendingUrlsValid = true;
        flushNativeWrites();
    } else {
        saveLegacy(urls, d->conferences);
    }
}

void BookmarksManager::saveLegacy(const QList<BookmarkUrl> &urls,
                                  const QList<NativeBookmark> &conferences)
{
    if (!d->online)
        return;
    if (d->legacyWriting) {
        d->legacyQueued = true;
        d->queuedLegacyUrls = urls;
        d->queuedLegacyConferences = conferences;
        return;
    }
    d->legacyWriting = true;
    const auto gen = d->generation;
    auto *task = new PrivateBookmarksTask(d->client->rootTask());
    connect(task, &Task::finished, this, [this, task, gen] {
        if (gen != d->generation || !d->online)
            return;
        d->legacyWriting = false;
        if (!task->success()) {
            emit syncError(QStringLiteral("Private XML bookmark save failed"));
        } else {
        if (d->urls != task->urls()) {
            d->urls = task->urls();
            emit urlsChanged();
        }
        if (!d->nativeWasActive && !same(d->conferences, task->conferences())) {
            d->conferences = task->conferences();
            emit conferencesChanged();
        }
        emit bookmarksSaved();
        }
        if (d->legacyQueued) {
            d->legacyQueued = false;
            const auto pendingUrls = d->queuedLegacyUrls;
            const auto pendingRooms = d->queuedLegacyConferences;
            saveLegacy(pendingUrls, pendingRooms);
        } else {
            flushNativeWrites();
        }
    });
    task->set(urls, conferences);
    task->go(true);
}

void BookmarksManager::flushNativeWrites()
{
    if (!d->nativeActive || d->replica.state() != PubSubReplica<NativeBookmark>::State::Ready)
        return;
    const auto batch = d->replica.beginWrite();
    if (!batch.isValid()) {
        if (d->pendingUrlsValid && !d->legacyWriting) {
            const auto urls = d->pendingUrls;
            d->pendingUrlsValid = false;
            // XEP-0402 doesn't define URLs. Keep them in legacy private XML,
            // retaining the current conference projection for #compat servers.
            saveLegacy(urls, d->conferences);
        }
        return;
    }
    auto remaining = std::make_shared<int>(batch.changes.size());
    auto failed = std::make_shared<bool>(false);
    for (auto it = batch.changes.cbegin(); it != batch.changes.cend(); ++it) {
        Task *task = nullptr;
        if (it.value()) {
            QDomDocument xml;
            task = d->client->pubSubManager()->publish(Jid(), NativeBookmark::node(),
                       it.value()->toItem(xml), NativeBookmark::privatePublishOptions());
        } else {
            task = d->client->pubSubManager()->retract(Jid(), NativeBookmark::node(), it.key());
        }
        connect(task, &Task::finished, this, [this, task, batch, remaining, failed] {
            if (!task->success())
                *failed = true;
            if (--*remaining != 0)
                return;
            if (!d->replica.finishWrite(batch, !*failed))
                return;
            if (*failed) {
                d->nativeActive = false;
                fetchNative();
                return;
            }
            rebuildNativeConferences();
            emit bookmarksSaved();
            flushNativeWrites();
        });
        task->go(true);
    }
}

} // namespace XMPP
