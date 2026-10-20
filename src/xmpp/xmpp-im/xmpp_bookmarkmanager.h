/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef XMPP_BOOKMARKMANAGER_H
#define XMPP_BOOKMARKMANAGER_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/xmpp_bookmarks.h>
#include <iris/xmpp-im/xmpp_pubsubreplica.h>

#include <QObject>
#include <QList>
#include <QString>
#include <memory>

namespace XMPP {
class Client;

/** XEP-0048 private-XML URL bookmark (not expressible in XEP-0402). */
struct BookmarkUrl {
    QString name;
    QString url;
    bool operator==(const BookmarkUrl &o) const { return name == o.name && url == o.url; }
};

/**
 * Shared bookmark protocol manager. Owns all network traffic, server feature
 * selection, PEP item reconciliation, and legacy private-XML compatibility.
 * Does not know about Qt widgets or client-local MUC auto-join policies.
 *
 * A host explicitly reports its authenticated session availability via
 * setOnline(). That makes the lifecycle unambiguous across reconnects.
 */
class IRIS_EXPORT BookmarksManager : public QObject {
    Q_OBJECT
public:
    explicit BookmarksManager(Client *client, QObject *parent = nullptr);
    ~BookmarksManager() override;

    bool isAvailable() const;
    bool usesNativeBookmarks() const;
    const QList<NativeBookmark> &conferences() const;
    const QList<BookmarkUrl> &urls() const;

    void setOnline(bool online);
    // Replace both legacy collections with one IQ; native PEP conferences
    // remain item-scoped while URLs still use Private XML.
    void setBookmarks(const QList<BookmarkUrl> &urls, const QList<NativeBookmark> &conferences);
    void setConferences(const QList<NativeBookmark> &conferences);
    // Intent-based edits: unrelated PEP changes made after baseline are kept.
    void applyConferenceEdits(const QList<NativeBookmark> &baseline,
                              const QList<NativeBookmark> &edited);
    void setUrls(const QList<BookmarkUrl> &urls);

signals:
    void availabilityChanged(bool available);
    void conferencesChanged();
    void urlsChanged();
    void bookmarksSaved();
    void syncError(const QString &message);

private:
    void fetchLegacy();
    void maybeEnableNative();
    void fetchNative();
    void rebuildNativeConferences();
    void flushNativeWrites();
    void saveLegacy(const QList<BookmarkUrl> &urls, const QList<NativeBookmark> &conferences);
    void scheduleNativeRetry();
    void setAvailable(bool available);

    class Private;
    std::unique_ptr<Private> d;
};
} // namespace XMPP
#endif // XMPP_BOOKMARKMANAGER_H
