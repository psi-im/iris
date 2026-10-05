#include <iris/jingle-pub.h>
#include <iris/jingle.h>
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>
#include <iris/xmpp_status.h>
#include <iris/xmpp_tasks.h>

#include <QtCrypto>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QTimer>

#include <functional>
#include <utility>

using namespace XMPP;
namespace J = XMPP::Jingle;

namespace {

constexpr auto PublicationNode = "urn:xmpp:jinglepub:ci";
constexpr auto PublicationId   = "jinglepub-prosody-durable";

class XmppEndpoint final : public QObject {
public:
    XmppEndpoint(Jid jid, QString password, QObject *parent = nullptr) :
        QObject(parent), jid_(std::move(jid)), password_(std::move(password))
    {
    }

    ~XmppEndpoint() override
    {
        if (stream_) {
            client_.close();
            delete stream_;
        }
        delete connector_;
    }

    Client *client() { return &client_; }

    void start(std::function<void()> ready, std::function<void(const QString &)> failed)
    {
        ready_  = std::move(ready);
        failed_ = std::move(failed);

        connector_ = new AdvancedConnector;
        connector_->setOptHostPort(QStringLiteral("127.0.0.1"), 5222);
        connector_->setOptSSL(false);

        stream_ = new ClientStream(connector_, nullptr);
        stream_->setAllowPlain(ClientStream::AllowPlain);
        stream_->setRequireMutualAuth(false);
        stream_->setCompress(false);
        stream_->setNoopTime(0);

        connect(stream_, &ClientStream::needAuthParams, this, [this](bool user, bool pass, bool realm) {
            if (user)
                stream_->setUsername(jid_.node());
            if (pass)
                stream_->setPassword(password_);
            if (realm)
                stream_->setRealm(jid_.domain());
            stream_->continueAfterParams();
        });
        connect(stream_, &ClientStream::warning, this, [this](int warning) {
            qInfo() << jid_.resource() << "XMPP warning" << warning << "- continuing in local CI mode";
            stream_->continueAfterWarning();
        });
        connect(stream_, &ClientStream::authenticated, this, [this]() {
            const Jid     bound    = stream_->jid();
            const QString resource = bound.resource().isEmpty() ? jid_.resource() : bound.resource();
            client_.start(jid_.domain(), jid_.node(), password_, resource);

            auto features = client_.features();
            for (const auto &feature : client_.jingleManager()->discoFeatures())
                features.addFeature(feature);
            client_.setFeatures(features);
            client_.setCaps(CapsSpec(QStringLiteral("https://iris-ci.local/jinglepub"), QCryptographicHash::Sha1));

            if (client_.isSessionRequired()) {
                auto *task = new JT_Session(client_.rootTask());
                connect(task, &Task::finished, this, [this, task]() {
                    if (!task->success()) {
                        fail(QStringLiteral("legacy XMPP session establishment failed"));
                        return;
                    }
                    becomeReady();
                });
                task->go(true);
            } else {
                becomeReady();
            }
        });
        connect(stream_, &Stream::error, this,
                [this](int error) { fail(QStringLiteral("XMPP stream error %1").arg(error)); });
        connect(stream_, &Stream::connectionClosed, this, [this]() {
            if (!readyState_)
                fail(QStringLiteral("XMPP stream closed before authentication"));
        });
        client_.connectToServer(stream_, jid_);
    }

private:
    void becomeReady()
    {
        if (readyState_)
            return;
        readyState_ = true;
        connect(&client_, &Client::xmlIncoming, this, [this](const QString &xml) {
            if (xml.contains(QStringLiteral("urn:xmpp:jinglepub:1"))
                || xml.contains(QStringLiteral("urn:xmpp:jingle:1"))) {
                qInfo().noquote() << jid_.resource() << "XMPP_IN=" << xml.trimmed();
            }
        });
        connect(&client_, &Client::xmlOutgoing, this, [this](const QString &xml) {
            if (xml.contains(QStringLiteral("urn:xmpp:jinglepub:1"))
                || xml.contains(QStringLiteral("urn:xmpp:jingle:1"))) {
                qInfo().noquote() << jid_.resource() << "XMPP_OUT=" << xml.trimmed();
            }
        });
        client_.setPresence(Status());
        qInfo().noquote() << QStringLiteral("XMPP_READY=%1").arg(client_.jid().full());
        if (ready_)
            ready_();
    }

    void fail(const QString &message)
    {
        if (failed_)
            failed_(message);
    }

    Client                    client_;
    Jid                       jid_;
    QString                   password_;
    AdvancedConnector        *connector_  = nullptr;
    ClientStream             *stream_     = nullptr;
    bool                      readyState_ = false;
    std::function<void()>     ready_;
    std::function<void(const QString &)> failed_;
};

class DurableProvider final : public J::PublishedSessionProvider {
public:
    DurableProvider(J::Manager *jingleManager, Jid owner, Jid publisher, QObject *parent = nullptr) :
        PublishedSessionProvider(parent), jingleManager_(jingleManager),
        endpoint_ { std::move(owner), QString::fromLatin1(PublicationNode), true }
    {
        publication_.setFrom(publisher);
        publication_.setId(QString::fromLatin1(PublicationId));
        publication_.setUri(QUrl(QStringLiteral("urn:uuid:jinglepub-prosody-durable")));
        publication_.addDescription(QStringLiteral("urn:xmpp:jingle:apps:file-transfer:5"));
    }

    J::JinglePub publication() const { return publication_; }
    bool         restored() const { return restored_; }

protected:
    QList<J::PublishedSessionEndpoint> publishedSessionEndpoints() const override { return { endpoint_ }; }

    void restoreCachedPublishedSessions() override
    {
        const auto cached = cachePublishedSession(
            endpoint_, publication_.id(), publication_, [manager = jingleManager_](const Jid &requester) {
                return manager ? manager->newSession(requester) : nullptr;
            });
        restored_ = cached.isValid();
        qInfo() << "DURABLE_PROVIDER_RESTORED=" << restored_ << "publication=" << publication_.id();
    }

    void publishedSessionObserved(const J::PublishedSessionEndpoint &, const QString &itemId,
                                  const J::JinglePub &publication) override
    {
        qInfo().noquote() << "DURABLE_PROVIDER_OBSERVED item=" << itemId
                          << "from=" << publication.from().full() << "id=" << publication.id();
    }

private:
    J::Manager                 *jingleManager_ = nullptr;
    J::PublishedSessionEndpoint endpoint_;
    J::JinglePub                publication_;
    bool                        restored_ = false;
};

XMPP::PubSubOptions publishOptions()
{
    return { { QStringLiteral("pubsub#access_model"), { QStringLiteral("whitelist") } },
             { QStringLiteral("pubsub#persist_items"), { QStringLiteral("1") } },
             { QStringLiteral("pubsub#max_items"), { QStringLiteral("10") } } };
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    if (argc != 3) {
        qCritical() << "Usage:" << argv[0] << "<bare-jid> <password>";
        return 2;
    }

    const Jid     owner(QString::fromLocal8Bit(argv[1]));
    const QString password = QString::fromLocal8Bit(argv[2]);
    if (!owner.isValid() || !owner.resource().isEmpty()) {
        qCritical() << "The integration test requires a valid bare JID";
        return 3;
    }

    const Jid publisherJid(owner.bare() + QStringLiteral("/publisher"));
    const Jid requesterJid(owner.bare() + QStringLiteral("/requester"));

    bool finishing = false;
    auto finish    = [&](int code, const QString &message) {
        if (finishing)
            return;
        finishing = true;
        if (code == 0)
            qInfo().noquote() << QStringLiteral("LIVE_JINGLEPUB_RESULT=success %1").arg(message);
        else
            qCritical().noquote() << QStringLiteral("LIVE_JINGLEPUB_RESULT=failure %1").arg(message);
        QTimer::singleShot(50, &app, [&, code]() { app.exit(code); });
    };

    QTimer::singleShot(25000, &app, [&]() {
        if (!finishing)
            finish(124, QStringLiteral("timeout"));
    });

    XmppEndpoint publisher(publisherJid, password, &app);
    XmppEndpoint requester(requesterJid, password, &app);

    auto *publicationManager = publisher.client()->jingleManager()->publicationManager();
    DurableProvider provider(publisher.client()->jingleManager(), owner, publisherJid, &app);
    publicationManager->registerProvider(&provider);
    if (!provider.restored()) {
        finish(10, QStringLiteral("could not cache durable publication before connection"));
        return app.exec();
    }

    bool publisherReady = false;
    bool requesterReady = false;
    bool nodeCreationStarted = false;
    bool publicationStarted  = false;
    bool requestStarted      = false;

    std::function<void()> advance;
    advance = [&]() {
        if (finishing || !publisherReady || !requesterReady)
            return;
        if (provider.state() == J::PublishedSessionProvider::State::Failed) {
            finish(20, QStringLiteral("durable provider synchronization failed"));
            return;
        }
        if (provider.state() != J::PublishedSessionProvider::State::Synchronized)
            return;

        if (!nodeCreationStarted) {
            nodeCreationStarted = true;
            auto *task = publisher.client()->pubSubManager()->createNode(owner, QString::fromLatin1(PublicationNode),
                                                                         publishOptions());
            connect(task, &Task::finished, &app, [&, task]() {
                if (!task->success()) {
                    finish(21, QStringLiteral("could not create Prosody PEP node"));
                    return;
                }
                advance();
            });
            task->go(true);
            return;
        }

        if (!publicationStarted) {
            publicationStarted = true;
            auto *task = publicationManager->publishSession(QString::fromLatin1(PublicationId), publishOptions());
            if (!task) {
                finish(22, QStringLiteral("publication manager refused durable publish"));
                return;
            }
            connect(task, &Task::finished, &app, [&, task]() {
                if (!task->success()) {
                    finish(23, QStringLiteral("Prosody rejected durable Jingle publication"));
                    return;
                }
                qInfo() << "DURABLE_PUBLISH_STATE="
                        << int(publicationManager->publishedSessionState(QString::fromLatin1(PublicationId)));
                // Give the self-PEP event/reconciliation path a chance to run.
                QTimer::singleShot(750, &app, advance);
            });
            task->go(true);
            return;
        }

        if (requestStarted)
            return;
        requestStarted = true;
        const auto state = publicationManager->publishedSessionState(QString::fromLatin1(PublicationId));
        qInfo() << "DURABLE_STATE_BEFORE_START=" << int(state)
                << "publisher-client-jid=" << publisher.client()->jid().full()
                << "publication-from=" << provider.publication().from().full();

        auto *remoteManager = requester.client()->jingleManager()->publicationManager();
        auto *request = remoteManager->requestPublishedSession(provider.publication().from(),
                                                               QString::fromLatin1(PublicationId), &app);
        connect(request, &J::PublishedSessionRequest::finished, &app, [&, request]() {
            if (request->state() != J::PublishedSessionRequest::State::Succeeded) {
                qCritical() << "PUBLISHED_START_FAILED condition=" << int(request->error().condition());
                finish(30, QStringLiteral("published-session start was rejected"));
                return;
            }
            qInfo().noquote() << "PUBLISHED_START_SID=" << request->sid();
            finish(0, QStringLiteral("durable Jingle publication survived Prosody authority reconciliation"));
        });
        request->start();
    };

    connect(&provider, &J::PublishedSessionProvider::stateChanged, &app,
            [&](J::PublishedSessionProvider::State state) {
                qInfo() << "DURABLE_PROVIDER_STATE=" << int(state);
                QTimer::singleShot(0, &app, advance);
            });

    publisher.start(
        [&]() {
            publisherReady = true;
            QTimer::singleShot(0, &app, advance);
        },
        [&](const QString &message) { finish(8, QStringLiteral("publisher: %1").arg(message)); });
    requester.start(
        [&]() {
            requesterReady = true;
            QTimer::singleShot(0, &app, advance);
        },
        [&](const QString &message) { finish(9, QStringLiteral("requester: %1").arg(message)); });

    return app.exec();
}
