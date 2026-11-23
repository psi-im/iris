/*
 * jingle-jet.h - protocol-independent Jingle Encrypted Transports (XEP-0391)
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#ifndef JINGLE_JET_H
#define JINGLE_JET_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/jingle-session.h>
#include <iris/xmpp-im/jingle.h>

#include <QByteArray>
#include <QDomDocument>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace XMPP { namespace Jingle { namespace JET {

    /**
     * Key material and its protected Jingle envelope. Encryption methods own the
     * envelope format; JET validates cipher/key sizes and installs the secret on
     * the content's transport.
     */
    struct IRIS_EXPORT ProtectedSecret {
        QByteArray                  key;
        QByteArray                  iv;
        QString                     cipher; // JET cipher URI
        QString                     type;   // encryption method's wire-profile URI
        QDomElement                 envelope;
        Session::PeerAuthentication authentication;
    };

    /**
     * Protocol-specific key exchange for XEP-0391. XEP-0396 (OMEMO) is one
     * implementation; another encryption method only needs another adapter.
     *
     * Non-owning registration: implementations must unregister before destruction.
     * A callback must be completed once even on cryptographic failure.
     */
    class IRIS_EXPORT KeyTransport {
    public:
        using Done = std::function<void(const std::optional<ProtectedSecret> &secret, const QString &error)>;

        virtual ~KeyTransport()                                                                           = default;
        virtual QString     methodId() const                                                              = 0;
        virtual QStringList discoFeatures() const                                                         = 0;
        virtual bool        canEncrypt(const Jid &peer) const                                             = 0;
        virtual bool        accepts(const QString &wireProfile) const                                     = 0;
        virtual void        encryptSecret(const Jid &peer, const Done &done)                              = 0;
        virtual void        decryptSecret(const Jid &peer, const QDomElement &security, const Done &done) = 0;
    };

    /** Shared JET signaling and per-content transport-key installation. */
    class IRIS_EXPORT Jet : public QObject, public SignalingFilter {
        Q_OBJECT
    public:
        static QString ns(); // urn:xmpp:jingle:jet:0
        static Jet    *of(Manager *manager);

        explicit Jet(Manager *manager);
        ~Jet() override;

        bool registerKeyTransport(KeyTransport *transport);
        void unregisterKeyTransport(KeyTransport *transport);

        bool        filterOutgoing(const Jid &to, const QDomElement &jingle, const Done &done) override;
        bool        filterIncoming(const Jid &from, const QDomElement &jingle, const Done &done) override;
        QStringList discoFeatures() const override;

    private:
        class Private;
        std::unique_ptr<Private> d;
    };

}}} // namespace XMPP::Jingle::JET
#endif // JINGLE_JET_H
