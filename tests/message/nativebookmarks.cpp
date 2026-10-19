/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <iris/xmpp_bookmarks.h>
#include <iris/xmpp_blocking.h>
#include <iris/xmpp_pubsubitem.h>

#include <QCoreApplication>
#include <QDomDocument>

using namespace XMPP;

static void check(bool ok, const char *reason)
{
    if (!ok)
        qFatal("%s", reason);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QDomDocument source;
    check(source.setContent(QStringLiteral(
        "<conference xmlns='urn:xmpp:bookmarks:1' name='Room' autojoin='true'>"
        "<nick>me</nick><password>secret</password><extensions>"
        "<state xmlns='urn:example:client-ext' favorite='yes'/></extensions></conference>"), true),
        "Cannot construct native bookmarks fixture");
    NativeBookmark original = NativeBookmark::fromItem(
        PubSubItem(QStringLiteral("room@conference.example.test"), source.documentElement()));
    check(original.isValid(), "XEP-0402 bookmark was rejected");
    check(original.autoJoin(), "autojoin lost");
    check(original.nick() == QStringLiteral("me"), "nick lost");
    check(original.password() == QStringLiteral("secret"), "password lost");

    original.setName(QStringLiteral("New room name"));
    original.setNick(QStringLiteral("new nickname"));
    QDomDocument roundtrip;
    const auto updated = original.toItem(roundtrip);
    check(updated.id() == QStringLiteral("room@conference.example.test"), "room item id changed");
    check(updated.payload().elementsByTagNameNS(QStringLiteral("urn:example:client-ext"),
                                                 QStringLiteral("state")).size() == 1,
          "Unknown extensions were dropped on republish");
    check(updated.payload().attribute(QStringLiteral("name")) == QStringLiteral("New room name"),
          "Updated name lost");
    check(NativeBookmark::fromItem(updated).nick() == QStringLiteral("new nickname"),
          "Updated nickname lost");

    const auto opts = NativeBookmark::privatePublishOptions();
    check(opts.value(QStringLiteral("pubsub#access_model")).contains(QStringLiteral("whitelist")),
          "PEP bookmarks could be published publicly");
    check(opts.value(QStringLiteral("pubsub#persist_items")).contains(QStringLiteral("true")),
          "PEP bookmarks were not configured for persistence");

    check(!NativeBookmark::fromItem(PubSubItem(QStringLiteral("bad/resource"),
                                               source.documentElement())).isValid(),
          "Invalid room item id was accepted");
    check(!NativeBookmark::fromItem(PubSubItem(QStringLiteral(""),
                                               source.documentElement())).isValid(),
          "Empty room item id was accepted");
    return 0;
}
