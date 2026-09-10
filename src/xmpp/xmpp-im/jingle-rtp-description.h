// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_RTP_DESCRIPTION_H
#define JINGLE_RTP_DESCRIPTION_H

#include <QDomElement>
#include <QList>
#include <QMap>
#include <iris/iris_export.h>
#include <optional>

namespace XMPP::Jingle::RTP {

struct PayloadType {
    quint8                 id = 0;
    QString                name;
    std::optional<quint32> clockrate;
    std::optional<quint8>  channels; // absent means one in a full offer/answer
    std::optional<quint32> ptime;
    std::optional<quint32> maxptime;
    QMap<QString, QString> parameters;
    // Extension XML (e.g. XEP-0293) is preserved, not implicitly negotiated.
    QList<QDomElement> extensions;
};

// Wire description only. Codec selection belongs to the media-provider contract;
// parsing this object does not authorize transmission or enable an RTP profile.
struct IRIS_EXPORT Description {
    QString                media;
    std::optional<quint32> ssrc;
    QList<PayloadType>     payloads;
    bool                   rtcpMux = false;
    QList<QDomElement>     extensions;

    static QString ns();
    // Advisory description-info can omit payloads or carry incomplete payloads.
    // It must not be treated as a replacement offer/answer.
    static std::optional<Description> fromXml(const QDomElement &, bool advisory = false);
    QDomElement                       toXml(QDomDocument &) const;
};

}
#endif
