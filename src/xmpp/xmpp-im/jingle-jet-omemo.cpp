/*
 * jingle-jet-omemo.cpp - Jingle Encrypted Transports with OMEMO (XEP-0391, XEP-0396)
 * Copyright (C) 2026  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "jingle-jet-omemo.h"

#include "jingle-ft.h"
#include "jingle-jet_p.h"
#include "jingle-session.h"
#include "xmpp_caps.h"
#include "xmpp_client.h"
#include "xmpp_encryption.h"
#include "xmpp_omemo.h"

#include <QHash>
#include <QPointer>

namespace XMPP { namespace Jingle {
    namespace {
        const QString JetNs(QStringLiteral("urn:xmpp:jingle:jet:0"));
        const QString JetOmemoNs(QStringLiteral("urn:xmpp:jingle:jet-omemo:0"));

        struct Secret {
            Origin                      creator = Origin::None;
            QByteArray                  key;
            QByteArray                  iv;
            Session::PeerAuthentication authentication;
        };

        QString key(const Jid &peer, const QString &sid) { return peer.full() + QLatin1Char('\n') + sid; }

        Origin creatorOf(const QDomElement &content)
        {
            return content.attribute(QStringLiteral("creator")) == QLatin1String("responder") ? Origin::Responder
                                                                                              : Origin::Initiator;
        }

        bool isFileTransfer(const QDomElement &content)
        {
            return content.firstChildElement(QStringLiteral("description")).namespaceURI() == FileTransfer::NS;
        }

        QDomElement securityOf(const QDomElement &content)
        {
            for (auto el = content.firstChildElement(QStringLiteral("security")); !el.isNull();
                 el      = el.nextSiblingElement(QStringLiteral("security")))
                if (el.namespaceURI() == JetNs)
                    return el;
            return {};
        }

        Stanza::Error refusal(const QString &text)
        {
            return Stanza::Error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::NotAcceptable, text);
        }

        void sequence(int count, const std::function<void(int, std::function<void()>)> &step,
                      const std::function<void()> &finished)
        {
            auto next = std::make_shared<std::function<void(int)>>();
            *next     = [count, step, finished, next](int index) {
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
    }

    class JetOmemo::Private {
    public:
        QPointer<Manager>                      manager;
        QPointer<OmemoEncryption>              omemo;
        QDomDocument                           document;
        QHash<QString, QHash<QString, Secret>> pending; // (peer, sid) → content name → secret
        QList<QPointer<Session>>               watched;

        // Gives received secrets to file-transfer contents that exist by now.
        void install(Session *session)
        {
            auto it = pending.find(key(session->peer(), session->sid()));
            if (it == pending.end())
                return;
            for (auto entry = it->begin(); entry != it->end();) {
                auto transfer
                    = qobject_cast<FileTransfer::Application *>(session->content(entry.key(), entry->creator));
                if (!transfer) {
                    ++entry;
                    continue;
                }
                transfer->setTransportSecret(entry->key, entry->iv);
                if (!session->peerAuthentication().isValid())
                    session->setPeerAuthentication(entry->authentication);
                entry = it->erase(entry);
            }
            if (it->isEmpty())
                pending.erase(it);
        }

        void watch(Session *session)
        {
            for (const auto &known : std::as_const(watched))
                if (known == session)
                    return;
            watched.append(session);
            QObject::connect(session, &Session::newContentReceived, session, [this, session]() { install(session); });
        }
    };

    QString JetOmemo::ns() { return JetOmemoNs; }

    QString JetOmemo::jetNs() { return JetNs; }

    JetOmemo::JetOmemo(Manager *manager, OmemoEncryption *omemo, QObject *parent) :
        QObject(parent), d(std::make_unique<Private>())
    {
        d->manager = manager;
        d->omemo   = omemo;
        manager->addSignalingFilter(this);
        connect(manager, &Manager::incomingSession, this, [this](Session *session) {
            d->install(session);
            d->watch(session);
        });
    }

    JetOmemo::~JetOmemo()
    {
        if (d->manager)
            d->manager->removeSignalingFilter(this);
    }

    QStringList JetOmemo::discoFeatures() const
    {
        if (!d->omemo || !d->omemo->supportedProtocols().testFlag(OmemoProtocol::Legacy)
            || !JET::StreamCipher::isSupported(16))
            return {};
        return { JetNs, JetOmemoNs };
    }

    bool JetOmemo::filterOutgoing(const Jid &to, const QDomElement &jingle, const Done &done)
    {
        const auto action = jingle.attribute(QStringLiteral("action"));
        if (action != QLatin1String("session-initiate") && action != QLatin1String("content-add"))
            return false;
        const auto sid     = jingle.attribute(QStringLiteral("sid"));
        const auto session = d->manager ? d->manager->session(to, sid) : nullptr;
        if (!session || !session->isEndToEndPreferred())
            return false;

        auto               copy = d->document.importNode(jingle, true).toElement();
        QList<QDomElement> contents;
        for (auto content = copy.firstChildElement(QStringLiteral("content")); !content.isNull();
             content      = content.nextSiblingElement(QStringLiteral("content")))
            if (isFileTransfer(content) && securityOf(content).isNull() && creatorOf(content) == session->role())
                contents.append(content);
        if (contents.isEmpty())
            return false;

        const bool required = session->isEndToEndRequired();
        const bool possible = d->omemo && d->omemo->isReady() && JET::StreamCipher::isSupported(16)
            && d->omemo->protocolsFor(to).testFlag(OmemoProtocol::Legacy)
            && d->manager->client()->capsManager()->features(to).test(JetOmemoNs);
        if (!possible) {
            if (!required)
                return false;
            done({}, refusal(QStringLiteral("The peer cannot receive end-to-end encrypted files (JET-OMEMO)")));
            return true;
        }

        QPointer<JetOmemo> guard(this);
        QPointer<Session>  sessionGuard(session);
        auto               failure = std::make_shared<std::optional<QString>>();
        auto               secrets = std::make_shared<QList<Secret>>();
        sequence(
            int(contents.size()),
            [this, guard, to, contents, failure, secrets](int index, std::function<void()> next) {
                auto job    = d->omemo->encryptElement(to, OmemoProtocol::Legacy, QDomElement());
                auto handle = [guard, job, content = contents.at(index), failure, secrets, next]() {
                    job->deleteLater();
                    if (!guard)
                        return;
                    const auto key = job->metadata().details.value(QStringLiteral("transportKey")).toByteArray();
                    const auto iv  = job->metadata().details.value(QStringLiteral("transportIv")).toByteArray();
                    if (!*failure && (!job->success() || key.size() != 16 || iv.size() != 12)) {
                        *failure = job->errorString();
                    } else if (!*failure) {
                        auto document = content.ownerDocument();
                        auto security = document.createElementNS(JetNs, QStringLiteral("security"));
                        security.setAttribute(QStringLiteral("name"), content.attribute(QStringLiteral("name")));
                        security.setAttribute(QStringLiteral("cipher"), JET::cipherFor(16));
                        security.setAttribute(QStringLiteral("type"), OmemoEncryption::legacyNamespaceUri());
                        security.appendChild(document.importNode(job->stanza(), true));
                        auto target = content;
                        target.appendChild(security);
                        secrets->append({ creatorOf(content), key, iv, {} });
                    }
                    next();
                };
                if (job->isFinished())
                    handle();
                else
                    connect(job, &EncryptionJob::finished, this, handle);
            },
            [guard, sessionGuard, copy, contents, failure, secrets, required, done]() {
                if (!guard)
                    return;
                if (*failure) {
                    if (required)
                        done({}, refusal(QStringLiteral("The file key could not be encrypted: %1").arg(**failure)));
                    else
                        done({}, std::nullopt); // unprotected, as without JET
                    return;
                }
                // The key is set before the offer leaves, so the transfer is encrypted from its start.
                for (int i = 0; sessionGuard && i < contents.size(); ++i) {
                    const auto name     = contents.at(i).attribute(QStringLiteral("name"));
                    auto       transfer = qobject_cast<FileTransfer::Application *>(
                        sessionGuard->content(name, secrets->at(i).creator));
                    if (transfer)
                        transfer->setTransportSecret(secrets->at(i).key, secrets->at(i).iv);
                }
                done(copy, std::nullopt);
            });
        return true;
    }

    bool JetOmemo::filterIncoming(const Jid &from, const QDomElement &jingle, const Done &done)
    {
        QList<QDomElement> contents;
        for (auto content = jingle.firstChildElement(QStringLiteral("content")); !content.isNull();
             content      = content.nextSiblingElement(QStringLiteral("content")))
            if (!securityOf(content).isNull())
                contents.append(content);
        if (contents.isEmpty())
            return false;
        if (!d->omemo) {
            done({}, refusal(QStringLiteral("End-to-end encrypted files are not supported")));
            return true;
        }

        const auto         sid = jingle.attribute(QStringLiteral("sid"));
        QPointer<JetOmemo> guard(this);
        auto               failure = std::make_shared<std::optional<QString>>();
        auto               secrets = std::make_shared<QHash<QString, Secret>>();
        sequence(
            int(contents.size()),
            [this, guard, from, contents, failure, secrets](int index, std::function<void()> next) {
                const auto content  = contents.at(index);
                const auto security = securityOf(content);
                const auto size     = JET::keySize(security.attribute(QStringLiteral("cipher")));
                if (*failure || security.attribute(QStringLiteral("type")) != OmemoEncryption::legacyNamespaceUri()
                    || size != 16
                    || security.attribute(QStringLiteral("name")) != content.attribute(QStringLiteral("name"))) {
                    if (!*failure)
                        *failure = QStringLiteral("Unsupported JET envelope or cipher");
                    next();
                    return;
                }
                auto job    = d->omemo->decryptElement(from, security.firstChildElement(QStringLiteral("encrypted")));
                auto handle = [guard, job, content, failure, secrets, next]() {
                    job->deleteLater();
                    if (!guard)
                        return;
                    const auto &meta = job->metadata();
                    const auto  key  = meta.details.value(QStringLiteral("transportKey")).toByteArray();
                    const auto  iv   = meta.details.value(QStringLiteral("transportIv")).toByteArray();
                    if (!*failure && (!job->success() || key.size() != 16 || iv.size() != 12)) {
                        *failure = QStringLiteral("The file key could not be decrypted: %1").arg(job->errorString());
                    } else if (!*failure) {
                        Session::PeerAuthentication auth;
                        auth.method      = QStringLiteral("omemo");
                        auth.deviceId    = meta.senderDeviceId;
                        auth.identityKey = meta.senderKey;
                        auth.trustLevel  = meta.details.value(QStringLiteral("trustLevel")).toInt();
                        secrets->insert(content.attribute(QStringLiteral("name")),
                                        { creatorOf(content), key, iv, auth });
                    }
                    next();
                };
                if (job->isFinished())
                    handle();
                else
                    connect(job, &EncryptionJob::finished, this, handle);
            },
            [this, guard, from, sid, failure, secrets, done]() {
                if (!guard)
                    return;
                if (*failure) {
                    done({}, refusal(**failure));
                    return;
                }
                auto &pending = d->pending[key(from, sid)];
                for (auto it = secrets->cbegin(); it != secrets->cend(); ++it)
                    pending.insert(it.key(), *it);
                // A content-add for an existing session gets its key once the content exists.
                if (auto session = d->manager ? d->manager->session(from, sid) : nullptr)
                    d->watch(session);
                done({}, std::nullopt);
            });
        return true;
    }
}}
