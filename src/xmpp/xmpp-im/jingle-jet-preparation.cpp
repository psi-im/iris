/*
 * jingle-jet-preparation.cpp - XEP-0391 content-scoped key preparation
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

#include "jingle-jet.h"
#include "jingle-jet_p.h"

#include <QPointer>

namespace XMPP { namespace Jingle { namespace JET {

    namespace {
        const QString JetNs(QStringLiteral("urn:xmpp:jingle:jet:0"));

        bool validSecret(const ProtectedSecret &secret)
        {
            const auto size = keySize(secret.cipher);
            return size && secret.key.size() == *size && secret.iv.size() == 12;
        }
    } // namespace

    class Jet::Private {
    public:
        QList<KeyTransport *> transports;

        KeyTransport *forMethod(const QString &methodId) const
        {
            for (auto *transport : transports) {
                if (transport->methodId() == methodId)
                    return transport;
            }
            return nullptr;
        }

        KeyTransport *forProfile(const QString &wireProfile) const
        {
            for (auto *transport : transports) {
                if (transport->accepts(wireProfile))
                    return transport;
            }
            return nullptr;
        }
    };

    QString Jet::ns() { return JetNs; }

    Jet::Jet(Manager *manager) : QObject(manager), d(std::make_unique<Private>())
    {
        Q_ASSERT(manager);
        manager->registerJet(this);
    }

    Jet::~Jet() = default;

    bool Jet::registerKeyTransport(KeyTransport *transport)
    {
        if (!transport || transport->methodId().isEmpty())
            return false;
        if (auto *existing = d->forMethod(transport->methodId()))
            return existing == transport;
        d->transports.append(transport);
        return true;
    }

    void Jet::unregisterKeyTransport(KeyTransport *transport)
    {
        if (!transport || !d->transports.removeAll(transport))
            return;
        emit keyTransportRemoved(transport->methodId());
    }

    QStringList Jet::discoFeatures() const
    {
        QStringList result;
        for (auto *transport : std::as_const(d->transports)) {
            const auto features = transport->discoFeatures();
            if (features.isEmpty())
                continue;
            if (result.isEmpty())
                result.append(JetNs);
            result.append(features);
        }
        result.removeDuplicates();
        return result;
    }

    void Jet::prepareOutgoing(Session *session, const Done &done)
    {
        if (!session) {
            done({}, QStringLiteral("Jingle session is unavailable"));
            return;
        }

        KeyTransport *transport = nullptr;
        if (!session->encryptionMethodId().isEmpty())
            transport = d->forMethod(session->encryptionMethodId());
        else if (d->transports.size() == 1)
            transport = d->transports.first();

        if (!transport || !session->allowsEncryptionMethod(transport->methodId())
            || !transport->canEncrypt(session->peer())) {
            done({}, QStringLiteral("No selected JET key transport is available for this peer"));
            return;
        }

        const QString methodId = transport->methodId();
        QPointer<Jet> guard(this);
        transport->encryptSecret(
            session->peer(),
            [guard, methodId, done](const std::optional<ProtectedSecret> &secret, const QString &error) {
                if (!guard)
                    return;
                auto *current = guard->d->forMethod(methodId);
                if (!current || !secret || !validSecret(*secret) || secret->type.isEmpty() || secret->envelope.isNull()
                    || !current->accepts(secret->type)) {
                    done({}, error.isEmpty() ? QStringLiteral("Invalid JET key exchange") : error);
                } else {
                    done(secret, {});
                }
            });
    }

    void Jet::prepareIncoming(Session *session, const QDomElement &security, const QString &contentName,
                              const Done &done)
    {
        if (!session || security.namespaceURI() != JetNs || security.attribute(QStringLiteral("name")) != contentName) {
            done({}, QStringLiteral("Invalid JET security envelope"));
            return;
        }
        const QString profile   = security.attribute(QStringLiteral("type"));
        const auto    keyBytes  = keySize(security.attribute(QStringLiteral("cipher")));
        auto         *transport = d->forProfile(profile);
        if (!transport || !session->allowsEncryptionMethod(transport->methodId()) || !keyBytes) {
            done({}, QStringLiteral("Unsupported or unselected JET key transport"));
            return;
        }
        const QString methodId = transport->methodId();
        QPointer<Jet> guard(this);
        transport->decryptSecret(session->peer(), security,
                                 [guard, methodId, profile, keyBytes,
                                  done](const std::optional<ProtectedSecret> &secret, const QString &error) {
                                     if (!guard)
                                         return;
                                     if (!guard->d->forMethod(methodId) || !secret || !validSecret(*secret)
                                         || secret->key.size() != *keyBytes || !secret->authentication.isValid()
                                         || secret->authentication.method != methodId || secret->type != profile) {
                                         done({}, error.isEmpty() ? QStringLiteral("Invalid JET key exchange") : error);
                                     } else {
                                         done(secret, {});
                                     }
                                 });
    }

}}} // namespace XMPP::Jingle::JET
