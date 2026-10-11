// SPDX-License-Identifier: LGPL-2.1-or-later
#include <iris/jingle-ft.h>
#include <iris/jingle-session.h>
#include <iris/tcpportreserver.h>
#include <iris/xmpp-im/jingle-ibb.h>
#include <iris/xmpp_client.h>

#include <QCoreApplication>
#include <QPointer>

using namespace XMPP;
namespace J  = XMPP::Jingle;
namespace FT = J::FileTransfer;

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

static void flushDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

// Session owns pads, pads own applications/transports. Applications only
// reference transports, so a transport lives while some application uses it.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TcpPortReserver  reserver;
    Client           client;
    client.setTcpPortReserver(&reserver);

    auto                *session = client.jingleManager()->newSession(Jid(QStringLiteral("peer@example.test/device")));
    QPointer<J::Session> sessionGuard(session);

    QPointer<J::Application> first(session->newContent(FT::NS, J::Origin::Initiator));
    QPointer<J::Application> second(session->newContent(FT::NS, J::Origin::Initiator));
    check(first && second, "file transfer contents were not created");
    QPointer<J::ApplicationManagerPad> appPad(first->pad());
    check(appPad && appPad->parent() == session && first->parent() == appPad && second->parent() == appPad,
          "applications are not owned through their session pad");

    QPointer<J::Transport> initial(session->newOutgoingTransport(J::IBB::NS));
    QPointer<J::Transport> shared(session->newOutgoingTransport(J::IBB::NS));
    check(initial && shared, "IBB transports were not created");
    QPointer<J::TransportManagerPad> transportPad(initial->pad());
    check(transportPad && transportPad->parent() == session && initial->parent() == transportPad,
          "transport is not owned through its session pad");

    // Replacing a transport releases the one which no longer has users.
    check(first->setTransport(initial), "initial transport was not installed");
    check(first->setTransport(shared), "replacement transport was not installed");
    flushDeletes();
    check(!initial, "replaced transport outlived its last application");
    check(shared && first->transport() == shared, "replacement transport was not retained");

    // A transport shared by several contents (BUNDLE) lives until its last user goes away.
    check(second->setTransport(shared), "shared transport was not installed for the second content");
    delete first.data();
    flushDeletes();
    check(shared && second->transport() == shared, "shared transport was released while still in use");
    delete second.data();
    flushDeletes();
    check(!shared, "shared transport outlived its last application");
    check(sessionGuard && appPad && transportPad, "pads did not survive removal of individual contents");

    // A candidate which was never installed is released explicitly, while a
    // used transport ignores the request.
    QPointer<J::Transport>   candidate(session->newOutgoingTransport(J::IBB::NS));
    QPointer<J::Transport>   used(session->newOutgoingTransport(J::IBB::NS));
    QPointer<J::Application> third(session->newContent(FT::NS, J::Origin::Initiator));
    check(candidate && used && third && third->setTransport(used), "candidate fixture was not created");
    candidate->releaseIfUnused();
    used->releaseIfUnused();
    flushDeletes();
    check(!candidate, "unused candidate transport was not released");
    check(used && third->transport() == used, "transport in use was released");

    // Session destruction tears down the whole tree; external handles do not retain it.
    delete session;
    check(!sessionGuard && !appPad && !transportPad && !third && !used,
          "pads, applications or transports outlived their session");
    flushDeletes();
    qInfo("Jingle transport ownership regressions passed");
    return 0;
}
