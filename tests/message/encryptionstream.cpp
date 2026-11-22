// SPDX-License-Identifier: LGPL-2.1-or-later
#include <iris/xmpp_encryption.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QDebug>

#include <cstdlib>

using namespace XMPP;

static void check(bool condition, const char *message)
{
    if (!condition) {
        qCritical() << message;
        std::abort();
    }
}

class StreamSession final : public EncryptedSession {
public:
    explicit StreamSession(const EncryptionContext &ctx) : EncryptedSession(QStringLiteral("test-stream"), ctx) { }

    EncryptionJob *doEncrypt(const QDomElement &xml) override
    {
        auto *job = new EncryptionJob(this);
        job->complete(xml);
        return job;
    }

    EncryptionJob *doEncryptChunk(const QByteArray &chunk, bool final) override
    {
        auto *job = new EncryptionJob(this);
        QTimer::singleShot(0, job, [job, chunk, final]() {
            job->complete(chunk.toUpper() + (final ? QByteArray("TAG") : QByteArray()));
        });
        return job;
    }

    EncryptionJob *doDecryptChunk(const QByteArray &chunk, bool final) override
    {
        auto *job = new EncryptionJob(this);
        QTimer::singleShot(0, job, [job, chunk, final]() {
            if (final && !chunk.endsWith("TAG"))
                job->fail(EncryptionJob::Error::AuthenticationFailed, QStringLiteral("Bad stream tag"));
            else
                job->complete(final ? chunk.left(chunk.size() - 3).toLower() : chunk.toLower());
        });
        return job;
    }
};

class StreamMethod final : public EncryptionMethod {
public:
    QString id() const override { return QStringLiteral("test-stream"); }
    QString name() const override { return QStringLiteral("Test stream"); }
    Capabilities capabilities() const override { return XmppStanza | DataStream; }
    EncryptedSession *startSession(Capabilities caps, const EncryptionContext &ctx) override
    {
        return caps ? new StreamSession(ctx) : nullptr;
    }
    Features features() const override { return {}; }
    bool canDecrypt(const QDomElement &) const override { return false; }
    Availability availability(const Jid &peer) const override
    {
        return peer.bare() == QStringLiteral("unsupported@example.test") ? Availability::Unavailable
                                                                          : Availability::Available;
    }
};

static EncryptionJob *await(EncryptionJob *job)
{
    if (!job->isFinished()) {
        QEventLoop loop;
        QObject::connect(job, &EncryptionJob::finished, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return job;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    EncryptionManager manager;
    StreamMethod method;
    check(manager.registerMethod(&method), "Cannot register stream method");
    check(manager.methods().count(method.id()) == 1, "Default registry query returned no methods");
    check(manager.availability(method.id(), Jid(QStringLiteral("online@example.test")))
              == EncryptionMethod::Availability::Available, "Registered method availability failed");
    check(manager.availability(method.id(), Jid(QStringLiteral("unsupported@example.test")))
              == EncryptionMethod::Availability::Unavailable, "Method-specific availability was ignored");
    check(manager.availability(QStringLiteral("missing"), Jid(QStringLiteral("online@example.test")))
              == EncryptionMethod::Availability::Unavailable, "Missing method was treated as available");
    const auto signalingAndStream = EncryptionMethod::Capabilities(EncryptionMethod::XmppStanza
                                                                  | EncryptionMethod::DataStream);
    check(manager.methods(signalingAndStream).count(method.id()) == 1,
          "Method supporting all required capabilities was not selected");
    check(manager.methods(EncryptionMethod::XmppStanza | EncryptionMethod::DataMessage).empty(),
          "Registry selected a method supporting only part of the required capabilities");

    // Jingle can use one encryption session both for signaling XML and for payload chunks.
    auto *tx = manager.startSession(method.id(),
                                    EncryptionMethod::Capabilities(EncryptionMethod::XmppStanza
                                                                   | EncryptionMethod::DataStream));
    check(tx != nullptr, "Cannot start combined signaling/stream session");
    QDomDocument doc;
    auto signaling = doc.createElement(QStringLiteral("jingle"));
    doc.appendChild(signaling);
    auto *signaled = await(tx->encrypt(signaling));
    check(signaled->success() && signaled->stanza().tagName() == QLatin1String("jingle"),
          "Shared session cannot protect signaling");
    auto *first = tx->encryptChunk(QByteArray("ab"));
    check(!first->isFinished(), "Expected asynchronous chunk");
    auto *overlap = tx->encryptChunk(QByteArray("cd"));
    check(overlap->isFinished() && !overlap->success(), "Overlapping chunks accepted");
    auto *wrongDirection = tx->decryptChunk(QByteArray("ab"));
    check(wrongDirection->isFinished() && !wrongDirection->success(), "Direction switch accepted");
    check(await(first)->success() && first->data() == "AB", "First chunk failed");
    auto *last = await(tx->encryptChunk(QByteArray("cd"), true));
    check(last->success() && last->data() == "CDTAG", "Final encryption chunk failed");
    check(!tx->encryptChunk(QByteArray("extra"))->success(), "Finalized stream reused");
    tx->close();

    auto *rx = manager.startSession(method.id(), EncryptionMethod::DataStream);
    auto *plain = await(rx->decryptChunk(QByteArray("AB")));
    check(plain->success() && plain->data() == "ab", "Intermediate decryption failed");
    auto *tampered = await(rx->decryptChunk(QByteArray("CDbad"), true));
    check(!tampered->success() && tampered->error() == EncryptionJob::Error::AuthenticationFailed,
          "Tampered final tag accepted");
    check(!rx->decryptChunk(QByteArray("TAG"), true)->success(), "Failed stream reused");
    rx->close();

    auto *ok = manager.startSession(method.id(), EncryptionMethod::DataStream);
    auto *verified = await(ok->decryptChunk(QByteArray("XYZTAG"), true));
    check(verified->success() && verified->data() == "xyz", "Valid stream not finalized");
    ok->close();

    auto *stanzaOnly = manager.startSession(method.id(), EncryptionMethod::XmppStanza);
    check(stanzaOnly != nullptr, "Stanza-only session not created");
    auto *wrongCapability = stanzaOnly->encryptChunk(QByteArray("plain"));
    check(wrongCapability->isFinished() && !wrongCapability->success(),
          "Stream operation accepted on a stanza-only session");
    stanzaOnly->close();
    check(!manager.startSession(method.id(), EncryptionMethod::DataMessage),
          "Unsupported data-message capability accepted");
    // Methods also create short-lived sessions directly (e.g. OMEMO for Jingle elements).
    // They must retain the same public encrypt() API without a registry-owned session.
    auto *directSession = method.startSession(EncryptionMethod::XmppStanza, {});
    check(directSession != nullptr, "Direct method session creation failed");
    auto *directResult = await(directSession->encrypt(signaling));
    check(directResult->success(), "Direct method session encryption failed");
    directSession->close();

    auto *registrySession = manager.startSession(method.id(), EncryptionMethod::DataStream);
    check(registrySession != nullptr, "Manager failed to create a session for unload test");
    manager.unregisterMethod(&method);
    check(registrySession->isClosing(), "Unregistering a method did not close its sessions");
    check(!registrySession->encryptChunk(QByteArray("more"))->success(),
          "A closed session accepted another operation");
    qInfo("Stream session routing and final authentication passed");
    return 0;
}
