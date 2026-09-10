// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp.h"
#include "jingle-nstransportslist.h"
#include "jingle-session.h"
#include <algorithm>

namespace XMPP::Jingle::RTP {
Pad::Pad(Manager *manager, Session *session, std::shared_ptr<MediaProvider> provider, QStringList transports) :
    manager_(manager), session_(session), provider_(std::move(provider)), transports_(std::move(transports))
{
    if (provider_)
        media_ = provider_->createSession();
}
Pad::~Pad() = default;
QString             Pad::ns() const { return Description::ns(); }
Session            *Pad::session() const { return session_; }
ApplicationManager *Pad::manager() const { return manager_; }
MediaSession       *Pad::mediaSession() const { return media_.get(); }
bool                Pad::incomingSessionInfo(const QDomElement &xml)
{
    if (!session_ || session_->state() >= State::Finishing)
        return false;
    QList<SessionInfo> infos;
    for (auto child = xml.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        auto info = SessionInfo::fromXml(child);
        if (!info)
            return false;
        if (!info->name.isEmpty()) {
            auto app = session_->content(info->name, info->creator);
            if (!app || app->pad().data() != this || app->state() >= State::Finishing)
                return false;
        }
        infos.append(*info);
    }
    if (infos.isEmpty())
        return false;
    QPointer<Pad> guard(this);
    for (const auto &info : infos) {
        if (!guard || !session_ || session_->state() >= State::Finishing)
            break;
        emit informationReceived(info);
    }
    return true;
}
QString Pad::generateContentName(Origin)
{
    QString name;
    do {
        name = QStringLiteral("rtp-%1").arg(++nextName_);
    } while (session_ && (session_->content(name, Origin::Initiator) || session_->content(name, Origin::Responder)));
    return name;
}

Application::Application(const QSharedPointer<Pad> &pad, const QString &name, Origin creator, Origin senders)
{
    _pad               = pad;
    _contentName       = name;
    _creator           = creator;
    _senders           = senders;
    _transportSelector = std::make_unique<NSTransportsList>(pad->session(), pad->transportNamespaces());
}
Application::~Application()
{
    stopMedia();
    if (_transport) {
        _transport->disconnect(this);
        _transport->stop();
    }
}
void Application::stopMedia()
{
    if (security_)
        security_->disconnect(this);
    security_.clear();
    attached_   = false;
    configured_ = false;
    negotiatedPayloads_.clear();
    if (endpoint_)
        endpoint_->stop();
}
void Application::setState(State state)
{
    if (_state == state)
        return;
    // Session rolls back accepted contents when a later content in the same
    // answer stanza is malformed. Restore the offer as well as the enum.
    if (state == State::Pending && _state == State::Accepted && beforeAnswer_) {
        negotiation_ = std::move(*beforeAnswer_);
        beforeAnswer_.reset();
    }
    _state = state;
    QPointer<Application> guard(this);
    if (state >= State::Finishing)
        stopMedia();
    if (guard)
        emit stateChanged(state);
}
bool Application::initializeOutgoing(const QString &media)
{
    if (isRemote() || endpoint_ || _state != State::Created || (media != "audio" && media != "video"))
        return false;
    auto pad = _pad.staticCast<Pad>();
    if (!pad->mediaSession())
        return false;
    auto endpoint = pad->mediaSession()->createEndpoint(_contentName, media);
    if (!endpoint)
        return false;
    auto offer = endpoint->localOffer();
    if (offer.media != media || negotiation_.setLocalOffer(offer) != Negotiation::Result::Ok) {
        endpoint->stop();
        return false;
    }
    endpoint_ = std::move(endpoint);
    return true;
}
Application::SetDescError Application::setRemoteOffer(const QDomElement &xml)
{
    if (!isRemote() || endpoint_ || _state != State::Created)
        return IncompatibleParameters;
    auto offer = Description::fromXml(xml);
    if (!offer)
        return Unparsed;
    auto pad = _pad.staticCast<Pad>();
    if ((offer->media != "audio" && offer->media != "video") || !pad->mediaSession())
        return IncompatibleParameters;
    auto endpoint = pad->mediaSession()->createEndpoint(_contentName, offer->media);
    if (!endpoint)
        return IncompatibleParameters;
    auto result = negotiation_.setRemoteOffer(*offer, *endpoint);
    if (result != Negotiation::Result::Ok) {
        endpoint->stop();
        return result == Negotiation::Result::InvalidDescription ? Unparsed : IncompatibleParameters;
    }
    endpoint_ = std::move(endpoint);
    return Ok;
}
Application::SetDescError Application::setRemoteAnswer(const QDomElement &xml)
{
    if (!endpoint_ || isRemote() || _state != State::Pending)
        return IncompatibleParameters;
    auto answer = Description::fromXml(xml);
    if (!answer)
        return Unparsed;
    if (endpoint_->supportsPacketIo() && !answer->rtcpMux)
        return IncompatibleParameters;
    auto candidate = negotiation_;
    auto result    = candidate.setRemoteAnswer(*answer, *endpoint_);
    if (result != Negotiation::Result::Ok)
        return result == Negotiation::Result::InvalidDescription ? Unparsed : IncompatibleParameters;
    beforeAnswer_ = negotiation_;
    negotiation_  = std::move(candidate);
    setState(State::Accepted);
    return Ok;
}
bool Application::incomingDescriptionInfo(const QDomElement &xml)
{
    if (!endpoint_ || _state >= State::Finishing)
        return false;
    auto hint  = Description::fromXml(xml, true);
    auto local = negotiation_.localDescription();
    if (!hint || !local || hint->media != local->media)
        return false;
    endpoint_->advisory(*hint);
    return true;
}
QDomElement Application::makeLocalOffer()
{
    auto description = negotiation_.localDescription();
    return description ? description->toXml(*_pad->doc()) : QDomElement();
}
QDomElement Application::makeLocalAnswer()
{
    return negotiation_.state() == Negotiation::State::Accepted ? makeLocalOffer() : QDomElement();
}
bool Application::isTransportReplaceEnabled() const
{
    // Coordinated replacement of a shared RTP connection is not implemented yet.
    return _state < State::Connecting;
}
void Application::prepare()
{
    if (!endpoint_ || (_state != State::Created && !(isRemote() && _state == State::Pending)))
        return;
    if (!_transport && !selectNextTransport())
        return;
    QPointer<Application> guard(this);
    setState(State::ApprovedToSend);
    if (guard)
        prepareTransport();
}
void Application::prepareTransport()
{
    if (!_transport)
        return;
    if (!endpoint_->supportsPacketIo()) {
        _transport->prepare();
        return;
    }
    if (security_)
        security_->disconnect(this);
    security_.clear();
    const auto local     = negotiation_.localDescription();
    const auto remote    = negotiation_.remoteDescription();
    auto       preparing = _transport;
    auto       packets   = dynamic_cast<PacketTransport *>(preparing.data());
    if (!local || !local->rtcpMux || (remote && !remote->rtcpMux) || !packets || !packets->enableRtpMux()) {
        remove(Reason::UnsupportedTransports, QStringLiteral("Authenticated RTP mux transport required"));
        return;
    }
    QPointer<Application> guard(this);
    preparing->prepare();
    if (!guard || _state >= State::Finishing || _transport != preparing)
        return;
    security_ = packets->rtpSession();
    if (!security_) {
        remove(Reason::SecurityError, QStringLiteral("RTP security binding unavailable"));
        return;
    }
    connect(security_, &SrtpSession::ready, this, &Application::activateMedia);
    connect(security_, &SrtpSession::invalidated, this,
            [this]() { remove(Reason::SecurityError, QStringLiteral("RTP security association invalidated")); });
    connect(security_, &QObject::destroyed, this,
            [this]() { remove(Reason::SecurityError, QStringLiteral("RTP security association destroyed")); });
    connect(
        security_, &SrtpSession::packetReceived, this,
        [this](const QByteArray &bytes, SrtpContext::Packet kind, quint64 epoch) {
            if (_state != State::Active || !attached_ || !security_ || !security_->isReady()
                || epoch != security_->epoch())
                return;
            if (kind == SrtpContext::Packet::Rtp
                && (!allowsRtp(false) || bytes.size() < 12 || !negotiatedPayloads_.contains(quint8(bytes[1]) & 0x7f)))
                return;
            endpoint_->receivePacket(bytes, kind);
        });
}
void Application::start()
{
    if (!endpoint_ || !_transport || configured_ || _state >= State::Finishing
        || (_state != State::Accepted && _state != State::Connecting)
        || negotiation_.state() != Negotiation::State::Accepted)
        return;
    const auto            local  = negotiation_.localDescription();
    const auto            remote = negotiation_.remoteDescription();
    QPointer<Application> guard(this);
    const bool            configured = endpoint_->configure(*local, *remote);
    if (!guard || _state >= State::Finishing)
        return;
    if (!configured) {
        remove(Reason::FailedApplication, QStringLiteral("Media configuration failed"));
        return;
    }
    configured_ = true;
    // Current negotiation retains offered PT identifiers, so the answer is the
    // accepted payload set in both directions, independent of offer preferences.
    for (const auto &payload : (isLocal() ? remote : local)->payloads)
        negotiatedPayloads_.insert(payload.id);
    beforeAnswer_.reset();
    auto transport = _transport;
    setState(State::Connecting);
    if (guard && _state == State::Connecting && configured_ && _transport == transport)
        transport->start();
    if (guard)
        activateMedia();
}
bool Application::allowsRtp(bool sending) const
{
    const auto localRole = _pad->session()->role();
    const auto role = sending ? localRole : (localRole == Origin::Initiator ? Origin::Responder : Origin::Initiator);
    return _senders == Origin::Both || _senders == role;
}
void Application::activateMedia()
{
    if (!configured_ || attached_ || _state != State::Connecting || !security_ || !security_->isReady())
        return;
    const auto            epoch = security_->epoch();
    QPointer<Application> guard(this);
    const bool            ok = endpoint_->attachPacketIo([guard, epoch](QByteArray data, SrtpContext::Packet kind) {
        return guard && guard->sendPacket(std::move(data), kind, epoch);
    });
    if (!guard)
        return;
    if (!ok) {
        remove(Reason::FailedApplication, QStringLiteral("Media packet attachment failed"));
        return;
    }
    if (_state != State::Connecting || !security_ || !security_->isReady() || security_->epoch() != epoch)
        return;
    attached_ = true;
    setState(State::Active);
}
bool Application::sendPacket(QByteArray data, SrtpContext::Packet kind, quint64 epoch)
{
    if (_state != State::Active || !attached_ || !security_ || !security_->isReady() || epoch != security_->epoch())
        return false;
    if (kind == SrtpContext::Packet::Rtp
        && (!allowsRtp(true) || data.size() < 12 || !negotiatedPayloads_.contains(quint8(data[1]) & 0x7f)))
        return false;
    auto packets = dynamic_cast<PacketTransport *>(_transport.data());
    return packets && packets->sendRtpPacket(std::move(data), kind, epoch);
}
void Application::remove(Reason::Condition condition, const QString &text)
{
    if (_state >= State::Finishing || stopping_)
        return;
    stopping_             = true;
    const auto finalState = isLocal() && _state <= State::ApprovedToSend ? State::Finished : State::Finishing;
    reason_ = _terminationReason = Reason(condition, text);
    QPointer<Application> guard(this);
    stopMedia();
    if (!guard)
        return;
    auto transport = _transport;
    if (transport) {
        transport->disconnect(this);
        transport->stop();
    }
    if (!guard)
        return;
    stopping_ = false;
    setState(finalState);
    if (guard)
        emit updated();
}
void Application::incomingRemove(const Reason &reason)
{
    if (_state >= State::Finishing || stopping_)
        return;
    stopping_ = true;
    reason_   = reason;
    QPointer<Application> guard(this);
    stopMedia();
    if (!guard)
        return;
    auto transport = _transport;
    if (transport)
        transport->stop();
    if (!guard)
        return;
    stopping_ = false;
    setState(State::Finished);
}

Manager::Manager(QObject *parent) : ApplicationManager(parent) { qRegisterMetaType<SessionInfo>(); }
Manager::~Manager() { closeAll(); }
void Manager::setJingleManager(XMPP::Jingle::Manager *manager)
{
    if (!manager)
        closeAll();
    jingle_ = manager;
}
void Manager::setMediaProvider(std::shared_ptr<MediaProvider> provider) { provider_ = std::move(provider); }
void Manager::setTransportNamespaces(const QStringList &transports) { transports_ = transports; }
ApplicationManagerPad *Manager::pad(Session *session)
{
    if (!provider_ || !session || session->manager() != jingle_)
        return nullptr;
    auto result = std::make_unique<Pad>(this, session, provider_, transports_);
    return result->mediaSession() ? result.release() : nullptr;
}
Application *Manager::startApplication(const ApplicationManagerPad::Ptr &base, const QString &name, Origin creator,
                                       Origin senders)
{
    auto pad = qSharedPointerDynamicCast<Pad>(base);
    if (!pad || pad->manager() != this || !pad->session() || !pad->mediaSession() || name.isEmpty()
        || (creator != Origin::Initiator && creator != Origin::Responder)
        || (senders != Origin::None && senders != Origin::Both && senders != Origin::Initiator
            && senders != Origin::Responder))
        return nullptr;
    auto app = new Application(pad, name, creator, senders);
    applications_.append(app);
    connect(app, &QObject::destroyed, this, [this] {
        applications_.erase(
            std::remove_if(applications_.begin(), applications_.end(), [](const auto &p) { return p.isNull(); }),
            applications_.end());
    });
    return app;
}
Application *Manager::createOutgoing(Session *session, const QString &media, Origin senders)
{
    if (!session || session->manager() != jingle_ || session->state() >= State::Finishing)
        return nullptr;
    auto pad = session->applicationPadFactory(Description::ns());
    if (!pad)
        return nullptr;
    std::unique_ptr<Application> app(
        startApplication(pad, pad->generateContentName(senders), session->role(), senders));
    if (!app || !app->initializeOutgoing(media))
        return nullptr;
    // addContent can prepare immediately, notifying user code which may delete
    // the session. Transfer ownership before entering those callbacks.
    auto                  content = app.release();
    QPointer<Application> guard(content);
    session->addContent(content);
    return guard;
}
void Manager::closeAll(const QString &)
{
    const auto applications = applications_;
    for (const auto &app : applications)
        if (app)
            app->remove(Reason::Gone, QStringLiteral("RTP manager stopped"));
}
}
