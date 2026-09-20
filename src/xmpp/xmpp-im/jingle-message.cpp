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
        if (!enabled_ || message.type() != Message::Type::Chat)
            return;

        for (const auto &initiation : message.jingleMessageInitiations())
            emit incoming(message, initiation);
    });
}

bool MessageInitiationManager::enabled() const { return enabled_; }

void MessageInitiationManager::setEnabled(bool enabled) { enabled_ = enabled; }

bool MessageInitiationManager::send(const Jid &to, const MessageInitiation &initiation)
{
    if (!enabled_ || !to.isValid() || !initiation.isValid())
        return false;

    Message message(to);
    message.setType(Message::Type::Chat);
    message.setProcessingHints(Message::Store);
    message.addJingleMessageInitiation(initiation);
    manager_->client()->sendMessage(message);
    return true;
}

}} // namespace XMPP::Jingle
