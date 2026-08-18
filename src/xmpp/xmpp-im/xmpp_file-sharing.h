/*
 * xmpp_file-sharing.h - XEP-0447 / XEP-0448 stateless file sharing
 * Copyright (C) 2026  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef XMPP_FILE_SHARING_H
#define XMPP_FILE_SHARING_H

#include "jingle-file.h"
#include "jingle-pub.h"
#include "xmpp_hash.h"

#include <QDomElement>
#include <QSharedPointer>
#include <QUrl>

#include <cstdint>
#include <optional>

namespace XMPP::StatelessFileSharing {

extern const QString NS;
extern const QString ENCRYPTED_NS;
extern const QString URL_DATA_NS;
extern const QString MESSAGE_ATTACHING_NS;

enum class Disposition { Unspecified, Inline, Attachment };
enum class Cipher { Unknown, Aes128Gcm, Aes256Gcm, Aes256CbcPkcs7 };

QString cipherUri(Cipher cipher);
Cipher  cipherFromUri(const QString &uri);
bool    cipherSupported(Cipher cipher);

class EncryptedSource;

class Source {
public:
    enum class Type { Invalid, UrlData, JinglePub, Encrypted, Other };

    Source();
    explicit Source(const QDomElement &element);

    static Source fromUrl(const QUrl &url);
    static Source fromJinglePub(const Jingle::JinglePub &publication);
    static Source fromEncrypted(const EncryptedSource &encrypted);
    static Source fromElement(const QDomElement &element);

    bool isValid() const;
    Type type() const;

    QUrl              url() const;
    Jingle::JinglePub jinglePub() const;
    EncryptedSource   encrypted() const;
    QDomElement       rawElement() const;

    bool        fromXml(const QDomElement &element);
    QDomElement toXml(QDomDocument *doc) const;

private:
    Type                            type_ = Type::Invalid;
    QUrl                            url_;
    Jingle::JinglePub               jinglePub_;
    QSharedPointer<EncryptedSource> encrypted_;
    QDomElement                     rawElement_;
};

class Sources {
public:
    Sources();
    explicit Sources(const QDomElement &element);

    bool isValid() const;
    bool isEmpty() const;

    QString id() const;
    void    setId(const QString &id);

    QList<Source> items() const;
    void          setItems(const QList<Source> &items);
    void          add(const Source &source);

    bool        fromXml(const QDomElement &element);
    QDomElement toXml(QDomDocument *doc) const;

private:
    QString       id_;
    QList<Source> items_;
};

class EncryptedSource {
public:
    EncryptedSource();
    explicit EncryptedSource(const QDomElement &element);

    bool isValid() const;

    Cipher cipher() const;
    void   setCipher(Cipher cipher);

    QByteArray key() const;
    void       setKey(const QByteArray &key);

    QByteArray iv() const;
    void       setIv(const QByteArray &iv);

    QList<Hash> hashes() const;
    void        setHashes(const QList<Hash> &hashes);
    void        addHash(const Hash &hash);

    Sources sources() const;
    void    setSources(const Sources &sources);

    bool        fromXml(const QDomElement &element);
    QDomElement toXml(QDomDocument *doc) const;

private:
    Cipher      cipher_ = Cipher::Unknown;
    QByteArray  key_;
    QByteArray  iv_;
    QList<Hash> hashes_;
    Sources     sources_;
};

class FileSharing {
public:
    FileSharing();
    explicit FileSharing(const QDomElement &element);

    bool isValid() const;

    Disposition disposition() const;
    void        setDisposition(Disposition disposition);

    QString id() const;
    void    setId(const QString &id);

    Jingle::FileTransfer::File file() const;
    void                       setFile(const Jingle::FileTransfer::File &file);

    Sources sources() const;
    void    setSources(const Sources &sources);

    bool        fromXml(const QDomElement &element);
    QDomElement toXml(QDomDocument *doc) const;

private:
    Disposition                disposition_ = Disposition::Unspecified;
    QString                    id_;
    Jingle::FileTransfer::File file_;
    Sources                    sources_;
};

struct EncryptedPayload {
    QByteArray data; // GCM payload includes the 16-byte authentication tag at the end.
    QByteArray key;
    QByteArray iv;
};

std::optional<EncryptedPayload> encrypt(Cipher cipher, const QByteArray &plaintext);
std::optional<QByteArray>       decrypt(Cipher cipher, const QByteArray &ciphertext, const QByteArray &key,
                                        const QByteArray &iv, std::optional<std::uint64_t> originalSize = {});

} // namespace XMPP::StatelessFileSharing

#endif // XMPP_FILE_SHARING_H
