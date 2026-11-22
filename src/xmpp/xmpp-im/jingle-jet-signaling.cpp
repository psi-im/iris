/*
 * jingle-jet-signaling.cpp - XEP-0391 key exchange and transport negotiation
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#include "jingle-jet.h"

#include "jingle-ft.h"
#include "jingle-jet_p.h"
#include "jingle-session.h"
#include "xmpp_client.h"

#include <QHash>
#include <QPointer>

#include <utility>

namespace XMPP { namespace Jingle { namespace JET {
namespace {
const QString JetNs(QStringLiteral("urn:xmpp:jingle:jet:0"));

QString pendingKey(const Jid &peer, const QString &sid)
{
    return peer.full() + QLatin1Char('\n') + sid;
}

Origin creatorOf(const QDomElement &content)
{
    return content.attribute(QStringLiteral("creator")) == QLatin1String("responder")
        ? Origin::Responder : Origin::Initiator;
}

bool isFileTransfer(const QDomElement &content)
{
    return content.firstChildElement(QStringLiteral("description")).namespaceURI() == FileTransfer::NS;
}

QDomElement securityOf(const QDomElement &content)
{
    for (auto element = content.firstChildElement(QStringLiteral("security")); !element.isNull();
         element = element.nextSiblingElement(QStringLiteral("security"))) {
        if (element.namespaceURI() == JetNs)
            return element;
    }
    return {};
}

Stanza::Error refusal(const QString &message)
{
    return Stanza::Error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::NotAcceptable, message);
}

bool validSecret(const ProtectedSecret &secret)
{
    const auto keyBytes = keySize(secret.cipher);
    return keyBytes && secret.key.size() == *keyBytes && secret.iv.size() == 12;
}

// Serialize asynchronous operations per content without blocking the event loop.
void sequence(int count, const std::function<void(int, std::function<void()>)> &step,
              const std::function<void()> &finished)
{
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [count, step, finished, next](int index) {
        if (index >= count) {
            finished();
            *next = nullptr;
            return;
        }
        step(index, [next, index]() {
            if (*next)
                (*next)(index + 1);
        });
    };
    (*next)(0);
}

struct StoredSecret {
    QString methodId;
    Origin creator = Origin::None;
    ProtectedSecret secret;
};
} // namespace

class Jet::Private {
public:
    QPointer<Manager> manager;
    QDomDocument document;
    QList<KeyTransport *> transports;
    QHash<QString, QHash<QString, StoredSecret>> pending;
    QList<QPointer<Session>> watched;

    KeyTransport *forMethod(const QString &methodId) const
    {
        for (auto *transport : transports)
            if (transport->methodId() == methodId)
                return transport;
        return nullptr;
    }

    KeyTransport *forProfile(const QString &wireProfile) const
    {
        for (auto *transport : transports)
            if (transport->accepts(wireProfile))
                return transport;
        return nullptr;
    }

    void install(Session *session)
    {
        auto it = pending.find(pendingKey(session->peer(), session->sid()));
        if (it == pending.end())
            return;
        for (auto entry = it->begin(); entry != it->end();) {
            if (!forMethod(entry->methodId) || !session->allowsEncryptionMethod(entry->methodId)) {
                entry = it->erase(entry);
                continue;
            }
            auto *transfer = qobject_cast<FileTransfer::Application *>(
                session->content(entry.key(), entry->creator));
            if (!transfer) {
                ++entry;
                continue;
            }
            const auto &secret = entry->secret;
            transfer->setTransportSecret(secret.key, secret.iv);
            if (!session->peerAuthentication().isValid() && secret.authentication.isValid())
                session->setPeerAuthentication(secret.authentication);
            entry = it->erase(entry);
        }
        if (it->isEmpty())
            pending.erase(it);
    }

    void watch(Session *session, QObject *owner)
    {
        for (const auto &known : std::as_const(watched))
            if (known == session)
                return;
        watched.append(session);
        const auto id = pendingKey(session->peer(), session->sid());
        QObject::connect(session, &Session::newContentReceived, owner,
                         [this, session]() { install(session); });
        QObject::connect(session, &QObject::destroyed, owner, [this, id]() {
            // QObject has already invalidated QPointers during destruction.
            watched.removeAll(QPointer<Session>());
            pending.remove(id);
        });
    }
};

QString Jet::ns() { return JetNs; }

Jet *Jet::of(Manager *manager)
{
    return manager ? manager->findChild<Jet *>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

Jet::Jet(Manager *manager) : QObject(manager), d(std::make_unique<Private>())
{
    d->manager = manager;
    // Register with the manager only while at least one crypto method exists.
    connect(manager, &Manager::incomingSession, this, [this](Session *session) {
        d->install(session);
        d->watch(session, this);
    });
}

Jet::~Jet()
{
    if (d->manager)
        d->manager->removeSignalingFilter(this);
}

bool Jet::registerKeyTransport(KeyTransport *transport)
{
    if (!transport || transport->methodId().isEmpty())
        return false;
    if (auto *existing = d->forMethod(transport->methodId()))
        return existing == transport;
    if (d->transports.isEmpty() && d->manager)
        d->manager->addSignalingFilter(this);
    d->transports.append(transport);
    return true;
}

void Jet::unregisterKeyTransport(KeyTransport *transport)
{
    if (!transport || !d->transports.removeAll(transport))
        return;
    // Do not retain uninstalled key material after its encryption method dies.
    for (auto pending = d->pending.begin(); pending != d->pending.end();) {
        for (auto entry = pending->begin(); entry != pending->end();) {
            if (entry->methodId == transport->methodId())
                entry = pending->erase(entry);
            else
                ++entry;
        }
        if (pending->isEmpty())
            pending = d->pending.erase(pending);
        else
            ++pending;
    }
    // Without a key transport this must not count as a Jingle security filter:
    // Manager's no-filter path rejects required E2EE for all content types.
    if (d->transports.isEmpty() && d->manager)
        d->manager->removeSignalingFilter(this);
}

QStringList Jet::discoFeatures() const
{
    QStringList result;
    for (auto *transport : std::as_const(d->transports)) {
        const auto advertised = transport->discoFeatures();
        if (advertised.isEmpty())
            continue;
        if (result.isEmpty())
            result.append(JetNs);
        result.append(advertised);
    }
    result.removeDuplicates();
    return result;
}

bool Jet::filterOutgoing(const Jid &to, const QDomElement &jingle, const Done &done)
{
    const auto action = jingle.attribute(QStringLiteral("action"));
    if (action != QLatin1String("session-initiate") && action != QLatin1String("content-add"))
        return false;

    const auto sid = jingle.attribute(QStringLiteral("sid"));
    auto *session = d->manager ? d->manager->session(to, sid) : nullptr;
    if (!session || !session->isEndToEndPreferred())
        return false;

    auto copy = d->document.importNode(jingle, true).toElement();
    QList<QDomElement> contents;
    for (auto content = copy.firstChildElement(QStringLiteral("content")); !content.isNull();
         content = content.nextSiblingElement(QStringLiteral("content"))) {
        if (isFileTransfer(content) && securityOf(content).isNull() && creatorOf(content) == session->role())
            contents.append(content);
    }
    if (contents.isEmpty())
        return false;

    const bool required = session->isEndToEndRequired();
    KeyTransport *transport = nullptr;
    if (!session->encryptionMethodId().isEmpty()) {
        transport = d->forMethod(session->encryptionMethodId());
    } else if (!d->transports.isEmpty()) {
        transport = d->transports.first(); // Historical unselected-session behavior.
    }
    if (!transport || !session->allowsEncryptionMethod(transport->methodId()) || !transport->canEncrypt(to)) {
        if (!required)
            return false;
        done({}, refusal(QStringLiteral("No selected JET key transport is available for this peer")));
        return true;
    }

    QPointer<Jet> guard(this);
    QPointer<Session> sessionGuard(session);
    auto failure = std::make_shared<std::optional<QString>>();
    auto secrets = std::make_shared<QList<ProtectedSecret>>();
    sequence(int(contents.size()),
        [guard, transport, to, contents, failure, secrets](int index, std::function<void()> next) {
            if (!guard || *failure) {
                next();
                return;
            }
            transport->encryptSecret(to, [guard, transport, content = contents.at(index), failure, secrets, next](
                                               const std::optional<ProtectedSecret> &secret, const QString &error) {
                if (!guard)
                    return;
                if (!secret || !validSecret(*secret) || secret->type.isEmpty() || secret->envelope.isNull()
                    || !transport->accepts(secret->type)) {
                    if (!*failure)
                        *failure = error.isEmpty() ? QStringLiteral("Invalid JET transport secret") : error;
                } else {
                    auto document = content.ownerDocument();
                    auto security = document.createElementNS(JetNs, QStringLiteral("security"));
                    security.setAttribute(QStringLiteral("name"), content.attribute(QStringLiteral("name")));
                    security.setAttribute(QStringLiteral("cipher"), secret->cipher);
                    security.setAttribute(QStringLiteral("type"), secret->type);
                    security.appendChild(document.importNode(secret->envelope, true));
                    auto target = content;
                    target.appendChild(security);
                    secrets->append(*secret);
                }
                next();
            });
        },
        [guard, sessionGuard, copy, contents, failure, secrets, required, done]() {
            if (!guard)
                return;
            if (*failure) {
                if (required)
                    done({}, refusal(QStringLiteral("JET key exchange failed: %1").arg(**failure)));
                else
                    done({}, std::nullopt); // Leave the unmodified offer for optional E2EE.
                return;
            }
            // Install the keys before sending the offer, never after a transfer begins.
            for (int i = 0; sessionGuard && i < contents.size(); ++i) {
                const auto name = contents.at(i).attribute(QStringLiteral("name"));
                auto *transfer = qobject_cast<FileTransfer::Application *>(
                    sessionGuard->content(name, sessionGuard->role()));
                if (transfer)
                    transfer->setTransportSecret(secrets->at(i).key, secrets->at(i).iv);
            }
            done(copy, std::nullopt);
        });
    return true;
}

bool Jet::filterIncoming(const Jid &from, const QDomElement &jingle, const Done &done)
{
    QList<QDomElement> contents;
    for (auto content = jingle.firstChildElement(QStringLiteral("content")); !content.isNull();
         content = content.nextSiblingElement(QStringLiteral("content"))) {
        if (!securityOf(content).isNull())
            contents.append(content);
    }
    if (contents.isEmpty())
        return false;

    const auto sid = jingle.attribute(QStringLiteral("sid"));
    auto *session = d->manager ? d->manager->session(from, sid) : nullptr;
    QPointer<Jet> guard(this);
    auto failure = std::make_shared<std::optional<QString>>();
    auto secrets = std::make_shared<QHash<QString, StoredSecret>>();
    sequence(int(contents.size()),
        [this, guard, from, session = QPointer<Session>(session), contents, failure, secrets](
            int index, std::function<void()> next) {
            if (!guard || *failure) {
                next();
                return;
            }
            const auto content = contents.at(index);
            const auto security = securityOf(content);
            const auto wireProfile = security.attribute(QStringLiteral("type"));
            auto *transport = d->forProfile(wireProfile);
            if (!transport || (session && !session->allowsEncryptionMethod(transport->methodId()))) {
                *failure = QStringLiteral("Unsupported or unselected JET key transport");
                next();
                return;
            }
            const auto cipherSize = keySize(security.attribute(QStringLiteral("cipher")));
            if (!cipherSize || security.attribute(QStringLiteral("name")) != content.attribute(QStringLiteral("name"))) {
                *failure = QStringLiteral("Unsupported JET envelope or cipher");
                next();
                return;
            }
            transport->decryptSecret(from, security, [guard, content, methodId = transport->methodId(),
                                                       wireProfile, cipherSize, failure, secrets, next](
                                                 const std::optional<ProtectedSecret> &secret, const QString &error) {
                if (!guard)
                    return;
                if (!secret || !validSecret(*secret) || secret->key.size() != *cipherSize
                    || !secret->authentication.isValid() || secret->type != wireProfile) {
                    if (!*failure)
                        *failure = error.isEmpty() ? QStringLiteral("Invalid JET key exchange") : error;
                } else {
                    secrets->insert(content.attribute(QStringLiteral("name")),
                                    { methodId, creatorOf(content), *secret });
                }
                next();
            });
        },
        [this, guard, from, sid, failure, secrets, done]() {
            if (!guard)
                return;
            if (*failure) {
                done({}, refusal(**failure));
                return;
            }
            auto &pending = d->pending[pendingKey(from, sid)];
            for (auto it = secrets->cbegin(); it != secrets->cend(); ++it)
                pending.insert(it.key(), *it);
            // content-add can arrive before its application is installed.
            if (auto *session = d->manager ? d->manager->session(from, sid) : nullptr) {
                d->install(session);
                d->watch(session, this);
            }
            done({}, std::nullopt);
        });
    return true;
}

}}} // namespace XMPP::Jingle::JET
