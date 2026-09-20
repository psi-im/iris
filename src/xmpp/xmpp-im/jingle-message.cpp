/*
 * jingle-message.cpp - XEP-0353 Jingle Message Initiation manager
 * Copyright (C) 2026 Sergey Ilinykh
 */

#include "jingle-message.h"

#include "jingle.h"
#include "xmpp_client.h"
#include "xmpp_message.h"

namespace XMPP { namespace Jingle {

MessageInitiationManager::MessageInitiationManager(Manager *manager) : QObject(manager), manager_(manager)
{
    Q_ASSERT(manager_);
    Q_ASSERT(manager_->client());

    connect(manager_->client(), &Client::messageReceived, this, [this](const Message &message) {
        // Carbons wrap the protocol message in a forwarding envelope. JMI state
        // is defined by the inner chat message, and sibling resources rely on
        // those carbon copies to stop ringing after proceed/reject.
        const auto effective = message.displayMessage();
        if (effective.type() != Message::Type::Chat)
            return;

        for (const auto &initiation : effective.jingleMessageInitiations())
            emit incoming(effective, initiation);
    });
}

bool MessageInitiationManager::enabled() const { return enabled_; }

void MessageInitiationManager::setEnabled(bool enabled) { enabled_ = enabled; }

bool MessageInitiationManager::send(const Jid &to, const MessageInitiation &initiation)
{
    // enabled_ controls discovery/new-proposal policy only. Existing JMI
    // lifecycles must still be able to send reject/finish if local media
    // capability disappears after the proposal was received.
    if (!to.isValid() || !initiation.isValid())
        return false;

    Message message(to);
    message.setType(Message::Type::Chat);
    message.setProcessingHints(Message::ProcessingHints(Message::Store));
    message.addJingleMessageInitiation(initiation);
    manager_->client()->sendMessage(message);
    return true;
}

}} // namespace XMPP::Jingle
