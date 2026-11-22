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

    EncryptionJob *encrypt(const QDomElement &xml) override
    {
        auto *job = new EncryptionJob(this);
        job->complete(xml);
        return job;
    }

    EncryptionJob *encryptChunk(const QByteArray &chunk, bool final) override
    {
        auto *job = new EncryptionJob(this);
        QTimer::singleShot(0, job, [job, chunk, final]() {
            job->complete(chunk.toUpper() + (final ? QByteArray("TAG") : QByteArray()));
        });
        return job;
    }

    EncryptionJob *decryptChunk(const QByteArray &chunk, bool final) override
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

    // Jingle can use one encryption session both for signaling XML and for payload chunks.
    auto *tx = manager.startSession(method.id(),
                                    EncryptionMethod::Capabilities(EncryptionMethod::XmppStanza
                                                                   | EncryptionMethod::DataStream));
    check(tx != nullptr, "Cannot start combined signaling/stream session");
    QDomDocument doc;
    auto signaling = doc.createElement(QStringLiteral("jingle"));
    doc.appendChild(signaling);
    auto *signaled = await(tx->run(signaling, EncryptedSession::Direction::Encrypt));
    check(signaled->success() && signaled->stanza().tagName() == QLatin1String("jingle"),
          "Shared session cannot protect signaling");
    auto *first = tx->run(QByteArray("ab"), EncryptedSession::Direction::Encrypt, false);
    check(!first->isFinished(), "Expected asynchronous chunk");
    auto *overlap = tx->run(QByteArray("cd"), EncryptedSession::Direction::Encrypt, false);
    check(overlap->isFinished() && !overlap->success(), "Overlapping chunks accepted");
    auto *wrongDirection = tx->run(QByteArray("ab"), EncryptedSession::Direction::Decrypt, false);
    check(wrongDirection->isFinished() && !wrongDirection->success(), "Direction switch accepted");
    check(await(first)->success() && first->data() == "AB", "First chunk failed");
    auto *last = await(tx->run(QByteArray("cd"), EncryptedSession::Direction::Encrypt, true));
    check(last->success() && last->data() == "CDTAG", "Final encryption chunk failed");
    check(!manager.encryptChunk(tx, QByteArray("extra"))->success(), "Finalized stream reused");
    tx->close();

    auto *rx = manager.startSession(method.id(), EncryptionMethod::DataStream);
    auto *plain = await(manager.decryptChunk(rx, QByteArray("AB")));
    check(plain->success() && plain->data() == "ab", "Intermediate decryption failed");
    auto *tampered = await(manager.decryptChunk(rx, QByteArray("CDbad"), true));
    check(!tampered->success() && tampered->error() == EncryptionJob::Error::AuthenticationFailed,
          "Tampered final tag accepted");
    check(!manager.decryptChunk(rx, QByteArray("TAG"), true)->success(), "Failed stream reused");
    rx->close();

    auto *ok = manager.startSession(method.id(), EncryptionMethod::DataStream);
    auto *verified = await(manager.decryptChunk(ok, QByteArray("XYZTAG"), true));
    check(verified->success() && verified->data() == "xyz", "Valid stream not finalized");
    ok->close();

    auto *stanzaOnly = manager.startSession(method.id(), EncryptionMethod::XmppStanza);
    check(stanzaOnly != nullptr, "Stanza-only session not created");
    auto *wrongCapability = stanzaOnly->run(QByteArray("plain"), EncryptedSession::Direction::Encrypt, false);
    check(wrongCapability->isFinished() && !wrongCapability->success(),
          "Stream operation accepted on a stanza-only session");
    stanzaOnly->close();
    check(!manager.startSession(method.id(), EncryptionMethod::DataMessage),
          "Unsupported data-message capability accepted");
    auto *registrySession = manager.startSession(method.id(), EncryptionMethod::DataStream);
    check(registrySession != nullptr, "Manager failed to create a session for unload test");
    manager.unregisterMethod(&method);
    check(registrySession->isClosing(), "Unregistering a method did not close its sessions");
    check(!registrySession->run(QByteArray("more"), EncryptedSession::Direction::Encrypt, false)->success(),
          "A closed session accepted another operation");
    qInfo("Stream session routing and final authentication passed");
    return 0;
}
