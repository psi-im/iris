// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_RTP_H
#define JINGLE_RTP_H

#include "jingle-application.h"
#include "jingle-rtp-info.h"
#include "jingle-rtp-negotiation.h"
#include "jingle-rtp-srtp.h"
#include <QPointer>
#include <QSet>
#include <functional>
#include <memory>

namespace XMPP::Jingle::RTP {

// All calls occur on the Jingle thread. Factories and negotiation must not
// capture media, start a nested event loop, or initiate network activity.
// The adapter owns its internal worker threads and must join them on destruction.
class IRIS_EXPORT MediaEndpoint : public CodecNegotiator {
public:
    using PacketWriter = std::function<bool(QByteArray, SrtpContext::Packet)>;
    // Stable opt-in capability; all calls, including PacketWriter, stay on the Jingle
    // thread. Worker-thread engines must use bounded queues in their adapter.
    virtual bool supportsPacketIo() const { return false; }
    // Called once after configure() and authentication. Returning true means
    // packet I/O is prepared, not permission to capture media. Writer becomes
    // usable when the Application is Active. stop() must detach all callbacks.
    virtual bool        attachPacketIo(PacketWriter) { return false; }
    virtual void        receivePacket(const QByteArray &, SrtpContext::Packet) { }
    virtual Description localOffer() const = 0;
    // Configure negotiated codecs without starting capture or packet transmission.
    virtual bool configure(const Description &local, const Description &remote) = 0;
    virtual void stop() = 0; // idempotent, synchronous quiescence of callbacks
    // Parsed partial hints, not a replacement offer. Ignoring a hint is valid.
    virtual void advisory(const Description &) { }
};

class IRIS_EXPORT MediaSession {
public:
    virtual ~MediaSession()                                                                                 = default;
    virtual std::unique_ptr<MediaEndpoint> createEndpoint(const QString &contentName, const QString &media) = 0;
};

class IRIS_EXPORT MediaProvider {
public:
    virtual ~MediaProvider()                              = default;
    virtual std::unique_ptr<MediaSession> createSession() = 0;
};

class Manager;
class IRIS_EXPORT Pad : public ApplicationManagerPad {
    Q_OBJECT
public:
    Pad(Manager *, Session *, std::shared_ptr<MediaProvider>, QStringList transports);
    ~Pad() override;
    QString             ns() const override;
    Session            *session() const override;
    ApplicationManager *manager() const override;
    QString             generateContentName(Origin) override;
    QStringList         sessionInfoNamespaces() const override { return { SessionInfo::ns() }; }
    bool                incomingSessionInfo(const QDomElement &) override;
    MediaSession       *mediaSession() const;
    const QStringList  &transportNamespaces() const { return transports_; }
signals:
    // Peer status only: never changes local consent or negotiated senders.
    void informationReceived(const XMPP::Jingle::RTP::SessionInfo &);

private:
    QPointer<Manager> manager_;
    QPointer<Session> session_;
    // Provider outlives its media session; endpoints outlive neither.
    std::shared_ptr<MediaProvider> provider_;
    std::unique_ptr<MediaSession>  media_;
    QStringList                    transports_;
    quint64                        nextName_ = 0;
};

class IRIS_EXPORT Application : public XMPP::Jingle::Application {
    Q_OBJECT
public:
    Application(const QSharedPointer<Pad> &, const QString &, Origin creator, Origin senders);
    ~Application() override;
    bool                                initializeOutgoing(const QString &media);
    void                                setState(State) override;
    const std::optional<Stanza::Error> &lastError() const override { return error_; }
    Reason                              lastReason() const override { return reason_; }
    SetDescError                        setRemoteOffer(const QDomElement &) override;
    SetDescError                        setRemoteAnswer(const QDomElement &) override;
    QDomElement                         makeLocalOffer() override;
    QDomElement                         makeLocalAnswer() override;
    bool                                supportsContentModify() const override { return true; }
    bool                                incomingDescriptionInfo(const QDomElement &) override;
    bool                                isTransportReplaceEnabled() const override;
    void                                prepare() override;
    void                                start() override;
    void                       remove(Reason::Condition = Reason::Success, const QString & = QString()) override;
    void                       incomingRemove(const Reason &) override;
    std::optional<Description> localDescription() const { return negotiation_.localDescription(); }
    std::optional<Description> remoteDescription() const { return negotiation_.remoteDescription(); }

protected:
    void prepareTransport() override;

private:
    void                           stopMedia();
    void                           activateMedia();
    bool                           sendPacket(QByteArray, SrtpContext::Packet, quint64 epoch);
    bool                           allowsRtp(bool sending) const;
    Negotiation                    negotiation_;
    std::optional<Negotiation>     beforeAnswer_;
    std::unique_ptr<MediaEndpoint> endpoint_;
    std::optional<Stanza::Error>   error_;
    Reason                         reason_;
    bool                           configured_ = false;
    bool                           attached_   = false;
    bool                           stopping_   = false;
    QPointer<SrtpSession>          security_;
    QSet<int>                      negotiatedPayloads_;
};

class IRIS_EXPORT Manager : public ApplicationManager {
    Q_OBJECT
public:
    explicit Manager(QObject *parent = nullptr);
    ~Manager() override;
    void setJingleManager(XMPP::Jingle::Manager *) override;
    void setMediaProvider(std::shared_ptr<MediaProvider>);
    // Explicit experimental transport whitelist. Empty by default until native
    // client interoperability is verified. Never used for discovery.
    void         setTransportNamespaces(const QStringList &);
    Application *createOutgoing(Session *, const QString &media, Origin senders = Origin::Both);
    Application *startApplication(const ApplicationManagerPad::Ptr &, const QString &, Origin, Origin) override;
    ApplicationManagerPad *pad(Session *) override;
    void                   closeAll(const QString & = QString()) override;
    QStringList            ns() const override { return { Description::ns() }; }
    QStringList            discoFeatures() const override { return {}; }

private:
    QPointer<XMPP::Jingle::Manager> jingle_;
    std::shared_ptr<MediaProvider>  provider_;
    QStringList                     transports_;
    QList<QPointer<Application>>    applications_;
};

}
#endif
