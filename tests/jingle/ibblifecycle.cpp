// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/xmpp_ibb.h"

#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>

#include <QCoreApplication>
#include <QPointer>
#include <QtCrypto>

using namespace XMPP;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class NoNetworkConnector final : public Connector {
public:
    void        setOptHostPort(const QString &, quint16) override { }
    void        connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void        done() override { }
};

class RecordingStream final : public ClientStream {
public:
    explicit RecordingStream(Connector *connector) : ClientStream(connector) { }
    void write(const Stanza &stanza) override { written.append(stanza.element().cloneNode(true).toElement()); }

    QList<QDomElement> written;
};

static void flushEvents()
{
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents();
}

static void acknowledgeLast(Client &client, const RecordingStream &stream, const Jid &peer)
{
    check(!stream.written.isEmpty(), "IBB fixture did not write an IQ");
    const auto sent = stream.written.constLast();
    auto       ack  = client.doc()->createElement(QStringLiteral("iq"));
    ack.setAttribute(QStringLiteral("from"), peer.full());
    ack.setAttribute(QStringLiteral("id"), sent.attribute(QStringLiteral("id")));
    ack.setAttribute(QStringLiteral("type"), QStringLiteral("result"));
    check(client.rootTask()->take(ack), "IBB fixture acknowledgement was not consumed");
    flushEvents();
}

int main(int argc, char **argv)
{
    QCoreApplication   app(argc, argv);
    QCA::Initializer   qca;
    NoNetworkConnector connector;
    RecordingStream    stream(&connector);
    Client             client;
    const Jid          peer(QStringLiteral("peer@example.test/device"));
    client.connectToServer(&stream, Jid(QStringLiteral("local@example.test/device")));

    auto *connection = static_cast<IBBConnection *>(client.ibbManager()->createConnection());
    check(connection, "IBB fixture could not create a connection");
    QPointer<IBBConnection> guard(connection);
    bool                    connected = false;
    QObject::connect(connection, &IBBConnection::connected, &app, [&]() { connected = true; });

    connection->connectToJid(peer, QStringLiteral("reentrant-close"));
    flushEvents();
    acknowledgeLast(client, stream, peer);
    check(connected && connection->isOpen(), "IBB fixture did not open the connection");

    const QByteArray payload("payload");
    check(connection->write(payload) == payload.size(), "IBB fixture could not queue payload");
    flushEvents();
    acknowledgeLast(client, stream, peer);

    QObject::connect(connection, &ByteStream::delayedCloseFinished, &app, [&]() {
        delete connection;
        connection = nullptr;
    });
    connection->close();
    flushEvents();
    acknowledgeLast(client, stream, peer);
    check(!guard && !connection, "IBB close completion did not exercise synchronous destruction");

    qInfo("IBB reentrant close regression passed");
    return 0;
}
