/*
 * jingle-message.h - XEP-0353 Jingle Message Initiation manager
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#ifndef JINGLE_MESSAGE_H
#define JINGLE_MESSAGE_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/xmpp_jinglemessage.h>

#include <QObject>

namespace XMPP {
class Jid;
class Message;

namespace Jingle {
class Manager;

class IRIS_EXPORT MessageInitiationManager : public QObject {
    Q_OBJECT
public:
    explicit MessageInitiationManager(Manager *manager);

    bool enabled() const;
    void setEnabled(bool enabled);

    bool send(const Jid &to, const MessageInitiation &initiation);

signals:
    void incoming(const XMPP::Message &message, const XMPP::Jingle::MessageInitiation &initiation);

private:
    Manager *manager_ = nullptr;
    bool     enabled_ = false;
};

} // namespace Jingle
} // namespace XMPP

#endif // JINGLE_MESSAGE_H
