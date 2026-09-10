// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp-description.h"
#include <QSet>
#include <limits>

namespace XMPP::Jingle::RTP {
QString Description::ns() { return QStringLiteral("urn:xmpp:jingle:apps:rtp:1"); }

static std::optional<quint32> number(const QString &text)
{
    if (text.isEmpty())
        return {};
    for (auto c : text) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            return {};
    }
    bool       ok    = false;
    const auto value = text.toULongLong(&ok);
    if (!ok || value > std::numeric_limits<quint32>::max())
        return {};
    return quint32(value);
}

std::optional<Description> Description::fromXml(const QDomElement &element, bool advisory)
{
    if (element.namespaceURI() != ns() || element.localName() != QLatin1String("description"))
        return {};
    Description result;
    result.media = element.attribute(QStringLiteral("media"));
    if (result.media.isEmpty())
        return {};
    if (element.hasAttribute(QStringLiteral("ssrc"))) {
        result.ssrc = number(element.attribute(QStringLiteral("ssrc")));
        if (!result.ssrc)
            return {};
    }
    QSet<int> ids;
    for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (child.namespaceURI() == ns() && child.localName() == QLatin1String("rtcp-mux")) {
            if (result.rtcpMux)
                return {};
            result.rtcpMux = true;
        } else if (child.namespaceURI() == ns() && child.localName() == QLatin1String("payload-type")) {
            PayloadType payload;
            const auto  id = number(child.attribute(QStringLiteral("id")));
            if (!id || *id > 127 || ids.contains(int(*id)))
                return {};
            ids.insert(int(*id));
            payload.id   = quint8(*id);
            payload.name = child.attribute(QStringLiteral("name"));
            if (!advisory && payload.id >= 96 && payload.name.isEmpty())
                return {};
            if (child.hasAttribute(QStringLiteral("channels"))) {
                const auto channels = number(child.attribute(QStringLiteral("channels")));
                if (!channels || *channels == 0 || *channels > 255)
                    return {};
                payload.channels = quint8(*channels);
            }
            for (auto entry : { qMakePair(QStringLiteral("clockrate"), &payload.clockrate),
                                qMakePair(QStringLiteral("ptime"), &payload.ptime),
                                qMakePair(QStringLiteral("maxptime"), &payload.maxptime) }) {
                if (child.hasAttribute(entry.first)) {
                    *entry.second = number(child.attribute(entry.first));
                    if (!*entry.second)
                        return {};
                }
            }
            for (auto param = child.firstChildElement(); !param.isNull(); param = param.nextSiblingElement()) {
                if (param.namespaceURI() == ns() && param.localName() == QLatin1String("parameter")) {
                    const auto name = param.attribute(QStringLiteral("name"));
                    if (name.isEmpty() || payload.parameters.contains(name)
                        || !param.hasAttribute(QStringLiteral("value")))
                        return {};
                    payload.parameters.insert(name, param.attribute(QStringLiteral("value")));
                } else {
                    payload.extensions.append(param.cloneNode(true).toElement());
                }
            }
            result.payloads.append(payload);
        } else {
            result.extensions.append(child.cloneNode(true).toElement());
        }
    }
    if (!advisory && result.payloads.isEmpty())
        return {};
    return result;
}

QDomElement Description::toXml(QDomDocument &doc) const
{
    auto root = doc.createElementNS(ns(), QStringLiteral("description"));
    root.setAttribute(QStringLiteral("media"), media);
    if (ssrc)
        root.setAttribute(QStringLiteral("ssrc"), QString::number(*ssrc));
    for (const auto &payload : payloads) {
        auto child = doc.createElementNS(ns(), QStringLiteral("payload-type"));
        child.setAttribute(QStringLiteral("id"), int(payload.id));
        if (!payload.name.isEmpty())
            child.setAttribute(QStringLiteral("name"), payload.name);
        if (payload.channels)
            child.setAttribute(QStringLiteral("channels"), int(*payload.channels));
        for (const auto &entry : { qMakePair(QStringLiteral("clockrate"), payload.clockrate),
                                   qMakePair(QStringLiteral("ptime"), payload.ptime),
                                   qMakePair(QStringLiteral("maxptime"), payload.maxptime) }) {
            if (entry.second)
                child.setAttribute(entry.first, QString::number(*entry.second));
        }
        for (auto it = payload.parameters.cbegin(); it != payload.parameters.cend(); ++it) {
            auto param = doc.createElementNS(ns(), QStringLiteral("parameter"));
            param.setAttribute(QStringLiteral("name"), it.key());
            param.setAttribute(QStringLiteral("value"), it.value());
            child.appendChild(param);
        }
        for (const auto &extension : payload.extensions)
            child.appendChild(doc.importNode(extension, true));
        root.appendChild(child);
    }
    if (rtcpMux)
        root.appendChild(doc.createElementNS(ns(), QStringLiteral("rtcp-mux")));
    for (const auto &extension : extensions)
        root.appendChild(doc.importNode(extension, true));
    return root;
}
}
