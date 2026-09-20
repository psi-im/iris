/*
 * xmpp_jinglemessage.cpp - XEP-0353 Jingle Message Initiation value type
 * Copyright (C) 2026 Sergey Ilinykh
 */

#include "xmpp_jinglemessage.h"

#include <QDomDocument>
#include <QDomElement>

namespace XMPP { namespace Jingle {
namespace {
const QString JmiNs(QStringLiteral("urn:xmpp:jingle-message:0"));
const QString JingleNs(QStringLiteral("urn:xmpp:jingle:1"));

QString localName(const QDomElement &element)
{
    return element.localName().isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : element.localName();
}

QByteArray serializeElement(const QDomElement &element)
{
    if (element.isNull())
        return {};
    QDomDocument owned;
    const auto   imported = owned.importNode(element, true);
    if (imported.isNull())
        return {};
    owned.appendChild(imported);
    return owned.toByteArray(-1);
}

QDomElement deserializeElement(QDomDocument &target, const QByteArray &xml)
{
    if (xml.isEmpty())
        return {};
    QDomDocument owned;
    if (!owned.setContent(xml, true))
        return {};
    const auto root = owned.documentElement();
    return root.isNull() ? QDomElement() : target.importNode(root, true).toElement();
}

MessageInitiation::Action actionFromName(const QString &name)
{
    if (name == QLatin1String("propose"))
        return MessageInitiation::Action::Propose;
    if (name == QLatin1String("ringing"))
        return MessageInitiation::Action::Ringing;
    if (name == QLatin1String("proceed"))
        return MessageInitiation::Action::Proceed;
    if (name == QLatin1String("reject"))
        return MessageInitiation::Action::Reject;
    if (name == QLatin1String("retract"))
        return MessageInitiation::Action::Retract;
    if (name == QLatin1String("finish"))
        return MessageInitiation::Action::Finish;
    return MessageInitiation::Action::None;
}

QString actionName(MessageInitiation::Action action)
{
    switch (action) {
    case MessageInitiation::Action::Propose:
        return QStringLiteral("propose");
    case MessageInitiation::Action::Ringing:
        return QStringLiteral("ringing");
    case MessageInitiation::Action::Proceed:
        return QStringLiteral("proceed");
    case MessageInitiation::Action::Reject:
        return QStringLiteral("reject");
    case MessageInitiation::Action::Retract:
        return QStringLiteral("retract");
    case MessageInitiation::Action::Finish:
        return QStringLiteral("finish");
    case MessageInitiation::Action::None:
        break;
    }
    return {};
}
} // namespace

MessageInitiation::MessageInitiation() = default;

MessageInitiation::MessageInitiation(Action action, const QString &id) : action_(action), id_(id) { }

const QString &MessageInitiation::ns() { return JmiNs; }

MessageInitiation MessageInitiation::fromXml(const QDomElement &element)
{
    MessageInitiation result;
    if (element.isNull() || element.namespaceURI() != JmiNs)
        return result;

    result.action_ = actionFromName(localName(element));
    result.id_     = element.attribute(QStringLiteral("id"));
    if (result.action_ == Action::None || result.id_.isEmpty())
        return MessageInitiation();

    for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        const auto name = localName(child);
        if (result.action_ == Action::Propose && name == QLatin1String("description")
            && !child.namespaceURI().isEmpty() && child.namespaceURI() != JmiNs) {
            Description description;
            description.ns    = child.namespaceURI();
            description.media = child.attribute(QStringLiteral("media"));
            description.xml   = serializeElement(child);
            result.descriptions_ += description;
            continue;
        }

        if (name == QLatin1String("reason") && child.namespaceURI() == JingleNs) {
            for (auto reasonChild = child.firstChildElement(); !reasonChild.isNull();
                 reasonChild      = reasonChild.nextSiblingElement()) {
                if (reasonChild.namespaceURI() != JingleNs)
                    continue;
                const auto reasonName = localName(reasonChild);
                if (reasonName == QLatin1String("text"))
                    result.reasonText_ = reasonChild.text();
                else if (result.reasonCondition_.isEmpty())
                    result.reasonCondition_ = reasonName;
            }
            continue;
        }

        if (name == QLatin1String("tie-break") && child.namespaceURI() == JmiNs) {
            result.tieBreak_ = true;
            continue;
        }

        if (name == QLatin1String("migrated") && child.namespaceURI() == JmiNs) {
            result.migratedTo_ = child.attribute(QStringLiteral("to"));
            continue;
        }

        const auto serialized = serializeElement(child);
        if (!serialized.isEmpty())
            result.extensions_ += serialized;
    }

    if (!result.isValid())
        return MessageInitiation();
    return result;
}

bool MessageInitiation::isValid() const
{
    return action_ != Action::None && !id_.isEmpty() && (action_ != Action::Propose || !descriptions_.isEmpty());
}

MessageInitiation::Action MessageInitiation::action() const { return action_; }

QString MessageInitiation::id() const { return id_; }

const QList<MessageInitiation::Description> &MessageInitiation::descriptions() const { return descriptions_; }

void MessageInitiation::addDescription(const QString &descriptionNs, const QString &media, const QByteArray &xml)
{
    descriptions_ += Description { descriptionNs, media, xml };
}

void MessageInitiation::setDescriptions(const QList<Description> &descriptions) { descriptions_ = descriptions; }

QString MessageInitiation::reasonCondition() const { return reasonCondition_; }

QString MessageInitiation::reasonText() const { return reasonText_; }

void MessageInitiation::setReason(const QString &condition, const QString &text)
{
    reasonCondition_ = condition;
    reasonText_      = text;
}

bool MessageInitiation::tieBreak() const { return tieBreak_; }

void MessageInitiation::setTieBreak(bool enabled) { tieBreak_ = enabled; }

QString MessageInitiation::migratedTo() const { return migratedTo_; }

void MessageInitiation::setMigratedTo(const QString &id) { migratedTo_ = id; }

const QList<QByteArray> &MessageInitiation::extensions() const { return extensions_; }

void MessageInitiation::addExtension(const QByteArray &xml)
{
    if (!xml.isEmpty())
        extensions_ += xml;
}

QDomElement MessageInitiation::toXml(QDomDocument *doc) const
{
    if (!doc || !isValid())
        return {};

    const auto name = actionName(action_);
    if (name.isEmpty())
        return {};

    auto element = doc->createElementNS(JmiNs, name);
    element.setAttribute(QStringLiteral("id"), id_);

    if (action_ == Action::Propose) {
        for (const auto &description : descriptions_) {
            QDomElement child = deserializeElement(*doc, description.xml);
            if (child.isNull()) {
                if (description.ns.isEmpty())
                    continue;
                child = doc->createElementNS(description.ns, QStringLiteral("description"));
                if (!description.media.isEmpty())
                    child.setAttribute(QStringLiteral("media"), description.media);
            }
            element.appendChild(child);
        }
    }

    if (!reasonCondition_.isEmpty() || !reasonText_.isEmpty()) {
        auto reason = doc->createElementNS(JingleNs, QStringLiteral("reason"));
        if (!reasonCondition_.isEmpty())
            reason.appendChild(doc->createElementNS(JingleNs, reasonCondition_));
        if (!reasonText_.isEmpty()) {
            auto text = doc->createElementNS(JingleNs, QStringLiteral("text"));
            text.appendChild(doc->createTextNode(reasonText_));
            reason.appendChild(text);
        }
        element.appendChild(reason);
    }

    if (tieBreak_)
        element.appendChild(doc->createElementNS(JmiNs, QStringLiteral("tie-break")));

    if (!migratedTo_.isEmpty()) {
        auto migrated = doc->createElementNS(JmiNs, QStringLiteral("migrated"));
        migrated.setAttribute(QStringLiteral("to"), migratedTo_);
        element.appendChild(migrated);
    }

    for (const auto &extensionXml : extensions_) {
        const auto extension = deserializeElement(*doc, extensionXml);
        if (!extension.isNull())
            element.appendChild(extension);
    }

    return element;
}

}} // namespace XMPP::Jingle
