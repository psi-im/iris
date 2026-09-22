// SPDX-License-Identifier: LGPL-2.1-or-later

#include <iris/ice176.h>

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QTimer>
#include <QtCrypto>

using namespace XMPP;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    Ice176 first;
    Ice176 second;

    const QList<Ice176::LocalAddress> localAddresses { Ice176::LocalAddress { QHostAddress::LocalHost } };
    for (auto *ice : { &first, &second }) {
        ice->setLocalAddresses(localAddresses);
        ice->setComponentCount(1);
        // Intentionally do not set remoteFeatures: RFC 8445 valid-pair
        // transmission is a local sending policy, not a negotiated capability.
        ice->setLocalFeatures(Ice176::Trickle | Ice176::NotNominatedData);
    }

    QList<Ice176::Candidate> firstCandidates;
    QList<Ice176::Candidate> secondCandidates;
    bool firstStarted = false;
    bool secondStarted = false;

    QObject::connect(&first, &Ice176::localCandidatesReady, [&](const QList<Ice176::Candidate> &candidates) {
        firstCandidates += candidates;
    });
    QObject::connect(&second, &Ice176::localCandidatesReady, [&](const QList<Ice176::Candidate> &candidates) {
        secondCandidates += candidates;
    });
    QObject::connect(&first, &Ice176::started, [&]() { firstStarted = true; });
    QObject::connect(&second, &Ice176::started, [&]() { secondStarted = true; });

    first.start(Ice176::Initiator);
    second.start(Ice176::Responder);

    QEventLoop gatherLoop;
    QTimer gatherPoll;
    QTimer gatherDeadline;
    gatherDeadline.setSingleShot(true);
    QObject::connect(&gatherDeadline, &QTimer::timeout, &gatherLoop, &QEventLoop::quit);
    QObject::connect(&gatherPoll, &QTimer::timeout, &gatherLoop, [&]() {
        if (firstStarted && secondStarted && !firstCandidates.isEmpty() && !secondCandidates.isEmpty())
            gatherLoop.quit();
    });
    gatherPoll.start(5);
    gatherDeadline.start(3000);
    gatherLoop.exec();
    gatherPoll.stop();

    check(firstStarted && secondStarted, "ICE agents did not start");
    check(!firstCandidates.isEmpty() && !secondCandidates.isEmpty(), "ICE agents did not gather host candidates");

    first.setRemoteCredentials(second.localUfrag(), second.localPassword());
    second.setRemoteCredentials(first.localUfrag(), first.localPassword());
    first.addRemoteCandidates(secondCandidates);
    second.addRemoteCandidates(firstCandidates);

    bool firstSelected = false;
    bool secondSelected = false;
    bool firstValidWritable = false;
    bool secondValidWritable = false;
    bool sentBeforeNomination = false;
    bool receivedBeforeNomination = false;
    bool sentAfterSelection = false;
    bool receivedAfterSelection = false;

    const QByteArray early = QByteArrayLiteral("valid-pair-before-nomination");
    const QByteArray final = QByteArrayLiteral("selected-pair-after-nomination");

    QObject::connect(&first, &Ice176::componentReady, [&](int component) {
        check(component == 0, "unexpected first ICE component");
        firstSelected = true;
    });
    QObject::connect(&second, &Ice176::componentReady, [&](int component) {
        check(component == 0, "unexpected second ICE component");
        secondSelected = true;
    });

    QObject::connect(&first, &Ice176::readyToSendMedia, [&]() {
        firstValidWritable = true;
        check(!firstSelected, "first ICE agent became writable only after nomination");
        sentBeforeNomination = true;
        first.writeDatagram(0, early);
    });
    QObject::connect(&second, &Ice176::readyToSendMedia, [&]() {
        secondValidWritable = true;
        check(!secondSelected, "second ICE agent became writable only after nomination");
    });

    QObject::connect(&second, &Ice176::readyRead, [&](int component) {
        check(component == 0, "unexpected readable ICE component");
        while (second.hasPendingDatagrams(component)) {
            const auto datagram = second.readDatagram(component);
            if (datagram == early)
                receivedBeforeNomination = true;
            else if (datagram == final)
                receivedAfterSelection = true;
        }
    });

    first.startChecks();
    second.startChecks();

    QEventLoop exchangeLoop;
    QTimer exchangePoll;
    QTimer exchangeDeadline;
    exchangeDeadline.setSingleShot(true);
    QObject::connect(&exchangeDeadline, &QTimer::timeout, &exchangeLoop, &QEventLoop::quit);
    QObject::connect(&exchangePoll, &QTimer::timeout, &exchangeLoop, [&]() {
        if (!sentAfterSelection && receivedBeforeNomination && firstSelected && secondSelected) {
            sentAfterSelection = true;
            first.writeDatagram(0, final);
        }
        if (receivedAfterSelection)
            exchangeLoop.quit();
    });
    exchangePoll.start(5);
    exchangeDeadline.start(8000);
    exchangeLoop.exec();
    exchangePoll.stop();

    check(firstValidWritable && secondValidWritable, "ICE did not expose a writable valid pair");
    check(sentBeforeNomination && receivedBeforeNomination, "data did not cross the pre-nomination valid pair");
    check(firstSelected && secondSelected, "ICE nomination did not eventually select a pair");
    check(sentAfterSelection && receivedAfterSelection, "data did not cross the selected pair");

    qInfo("RFC 8445 valid-pair data regression passed");
    return 0;
}
