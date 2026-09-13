// SPDX-License-Identifier: LGPL-2.1-or-later
#define main iris_transportreplace_legacy_main
#include "transportreplace.cpp"
#undef main

#include <functional>

class ReentrantAckTransport : public TestTransport {
public:
    using TestTransport::TestTransport;

    J::OutgoingTransportInfoUpdate takeOutgoingUpdate(bool ensureTransportElement) override
    {
        Q_UNUSED(ensureTransportElement);
        auto el = pad()->doc()->createElementNS(pad()->ns(), QStringLiteral("transport"));
        el.setAttribute(QStringLiteral("id"), id());
        setHasUpdates(false);
        return { el, [self = QPointer<ReentrantAckTransport>(this)](Task *task) {
                    if (!self)
                        return;
                    if (task && task->success())
                        self->forceState(J::State::Pending);
                    auto callback = std::move(self->onAck);
                    if (callback)
                        callback();
                } };
    }

    std::function<void()> onAck;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    TestTransportManager transportManager;
    Client client;
    client.jingleManager()->registerTransport(&transportManager);
    Result success(client.rootTask(), true);

    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto pad = session.transportPadFactory(TestTransportManager::namespaceUri());
    check(bool(pad), "test transport pad missing");

    auto remote = QSharedPointer<ReentrantAckTransport>::create(
        pad, J::Origin::Responder, QStringLiteral("remote"));
    remote->forceState(J::State::ApprovedToSend);
    remote->setHasUpdates(true);

    auto appPtr = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, remote,
                                 std::make_unique<TestSelector>(), nullptr, J::State::Connecting);
    appPtr->markReplaceInProgress();

    auto newerLocal = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("new-local"));
    remote->onAck = [appPtr, newerLocal]() {
        check(appPtr->setTransport(newerLocal), "reentrant transport-accept ACK could not install newer local transport");
        check(appPtr->replacePlanned(), "newer local transport was not marked as a planned replacement");
    };

    check(appPtr->evaluateOutgoingUpdate().action == J::Action::TransportAccept,
          "remote replacement did not evaluate to transport-accept");
    auto update = appPtr->takeOutgoingUpdate();
    const auto &ack = std::get<1>(update);
    check(bool(ack), "transport-accept had no ACK callback");
    ack(&success);

    check(appPtr->transport().data() == newerLocal.data(),
          "old transport-accept ACK replaced the newer reentrant transport");
    check(appPtr->replacePlanned(),
          "old transport-accept ACK cleared the newer replacement transaction");
    check(newerLocal->starts() == 0,
          "old transport-accept ACK started the newer replacement transport");

    qInfo("Transport-accept ACK reentrancy regression passed");
    return 0;
}
