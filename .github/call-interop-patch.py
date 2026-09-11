from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, got {count}")
    p.write_text(text.replace(old, new))


replace_once(
    "src/xmpp/xmpp-im/jingle-session.cpp",
    '''            typedef std::tuple<QPointer<Application>, OutgoingUpdateCB> AckHndl;
            if (role == Origin::Responder) {
                for (const auto &c : std::as_const(initialIncomingUnacceptedContent)) {
                    auto out = c->evaluateOutgoingUpdate();
                    if (out.action == Action::ContentReject) {
                        lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                        XMPP::Stanza::Error::ErrorCond::BadRequest);
                        setSessionFinished();
                        return true;
                    }
                    if (out.action != Action::ContentAccept) {
                        return false; // keep waiting.
                    }
                }
            } else {
''',
    '''            typedef std::tuple<QPointer<Application>, OutgoingUpdateCB, bool> AckHndl;
            QSet<Application *> rejectedInitialContent;
            if (role == Origin::Responder) {
                int    acceptedInitialContent = 0;
                Reason rejectionReason;
                for (const auto &c : std::as_const(initialIncomingUnacceptedContent)) {
                    auto out = c->evaluateOutgoingUpdate();
                    if (out.action == Action::ContentAccept) {
                        ++acceptedInitialContent;
                        continue;
                    }
                    if (out.action == Action::ContentReject || out.action == Action::ContentRemove) {
                        rejectedInitialContent.insert(c);
                        if (!rejectionReason.isValid() && out.reason.isValid())
                            rejectionReason = out.reason;
                        continue;
                    }
                    return false; // keep waiting.
                }
                if (!acceptedInitialContent) {
                    q->terminate(rejectionReason.isValid() ? rejectionReason.condition() : Reason::Decline,
                                 rejectionReason.text());
                    return true;
                }
            } else {
''',
    "initial content preflight",
)

replace_once(
    "src/xmpp/xmpp-im/jingle-session.cpp",
    '''            QList<QDomElement> contents;
            QList<AckHndl>     acceptApps;
            for (const auto &app : std::as_const(contentList)) {
                QList<QDomElement> xml;
                OutgoingUpdateCB   callback;
                std::tie(xml, callback) = app->takeOutgoingUpdate();
                contents += xml;
                // p->setState(State::Unacked);
                if (callback) {
                    acceptApps.append(AckHndl { app, callback });
                }
            }
''',
    '''            QList<QDomElement> contents;
            QList<AckHndl>     acceptApps;
            for (const auto &app : std::as_const(contentList)) {
                QList<QDomElement> xml;
                OutgoingUpdateCB   callback;
                std::tie(xml, callback) = app->takeOutgoingUpdate();
                const bool rejectedInitial
                    = role == Origin::Responder && rejectedInitialContent.contains(app);
                if (!rejectedInitial)
                    contents += xml;
                if (callback)
                    acceptApps.append(AckHndl { app, callback, !rejectedInitial });
            }
            if (contents.isEmpty()) {
                q->terminate(Reason::Decline, QStringLiteral("No initial content was accepted"));
                return true;
            }
''',
    "initial content serialization",
)

replace_once(
    "src/xmpp/xmpp-im/jingle-session.cpp",
    '''                    auto app      = std::get<0>(h);
                    auto callback = std::get<1>(h);
                    if (app) {
                        callback(jt);
                        if (role == Origin::Responder) {
                            app->start();
                        }
                    }
''',
    '''                    auto app         = std::get<0>(h);
                    auto callback    = std::get<1>(h);
                    auto shouldStart = std::get<2>(h);
                    if (app) {
                        callback(jt);
                        if (role == Origin::Responder && shouldStart)
                            app->start();
                    }
''',
    "initial content acknowledgement",
)

replace_once(
    "src/xmpp/xmpp-im/jingle-ice.h",
    '''        void setExternalAddress(const QString &host);
        void setSelfAddress(const QHostAddress &addr);
        void setStunBindService(const QString &host, int port);
''',
    '''        void setExternalAddress(const QString &host);
        void setSelfAddress(const QHostAddress &addr);
        void setAllowIpExposure(bool allow);
        void setStunBindService(const QString &host, int port);
''',
    "ICE privacy declaration",
)

replace_once(
    "src/xmpp/xmpp-im/jingle-ice.cpp",
    '''        int          basePort = -1;
        QString      extHost;
        QHostAddress selfAddr;

        QString stunBindHost;
''',
    '''        int          basePort        = -1;
        QString      extHost;
        QHostAddress selfAddr;
        bool         allowIpExposure = true;

        QString stunBindHost;
''',
    "ICE privacy state",
)

replace_once(
    "src/xmpp/xmpp-im/jingle-ice.cpp",
    '''            network->ice = new Ice176(network.data());

            q->connect(network->ice, &XMPP::Ice176::started, q, [this]() {
''',
    '''            network->ice = new Ice176(network.data());
            network->ice->setAllowIpExposure(manager->allowIpExposure);

            q->connect(network->ice, &XMPP::Ice176::started, q, [this]() {
''',
    "ICE privacy application",
)

replace_once(
    "src/xmpp/xmpp-im/jingle-ice.cpp",
    '''    void Manager::setExternalAddress(const QString &host) { d->extHost = host; }

    void Manager::setSelfAddress(const QHostAddress &addr) { d->selfAddr = addr; }

    void Manager::setStunBindService(const QString &host, int port)
''',
    '''    void Manager::setExternalAddress(const QString &host) { d->extHost = host; }

    void Manager::setSelfAddress(const QHostAddress &addr) { d->selfAddr = addr; }

    void Manager::setAllowIpExposure(bool allow) { d->allowIpExposure = allow; }

    void Manager::setStunBindService(const QString &host, int port)
''',
    "ICE privacy setter",
)
