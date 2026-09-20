/*
 * xmpp_jinglemessage.cpp - XEP-0353 Jingle Message Initiation value type
 * Copyright (C) 2026 Sergey Ilinykh
 */

#include "xmpp_jinglemessage.h"

#include <QDomDocument>
#include <QSharedData>

namespace XMPP { namespace Jingle {
namespace {
const QString JmiNs(QStringLiteral("urn:xmpp:jingle-message:0"));
const QString JingleNs(QStringLiteral("urn:xmpp:jingle:1"));

QString localName(const QDomElement &element)
{
    return element.localName().isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : element.localName();
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

class MessageInitiation::Private : public QSharedData {
public:
    Action             action = Action::None;
    QString            id;
    QDomDocument       descriptionsDocument;
    QList<QDomElement> descriptions;
    QString            reasonCondition;
    QString            reasonText;
    bool               tieBreak = false;
    QString            migratedTo;
    QDomDocument       extensionsDocument;
    QList<QDomElement> extensions;
};

MessageInitiation::MessageInitiation() = default;

MessageInitiation::MessageInitiation(Action action, const QString &id) : d(new Private)
{
    d->action = action;
    d->id     = id;
}

MessageInitiation::MessageInitiation(const MessageInitiation &) = default;
MessageInitiation &MessageInitiation::operator=(const MessageInitiation &) = default;
MessageInitiation::~MessageInitiation() = default;

MessageInitiation::Private *MessageInitiation::ensureD()
{
    if (!d)
        d = new Private;
    return d.data();
}

const QString &MessageInitiation::ns() { return JmiNs; }

MessageInitiation MessageInitiation::fromXml(const QDomElement &element)
{
    if (element.isNull() || element.namespaceURI() != JmiNs)
        return {};

    const auto action = actionFromName(localName(element));
    const auto id     = element.attribute(QStringLiteral("id"));
    if (action == Action::None || id.isEmpty())
        return {};

    MessageInitiation result(action, id);
    auto              data = result.ensureD();

    for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        const auto name = localName(child);
        if (action == Action::Propose && name == QLatin1String("description")
            && !child.namespaceURI().isEmpty() && child.namespaceURI() != JmiNs) {
            const auto imported = data->descriptionsDocument.importNode(child, true).toElement();
            if (!imported.isNull())
                data->descriptions.append(imported);
            continue;
        }

        if (name == QLatin1String("reason") && child.namespaceURI() == JingleNs) {
            for (auto reasonChild = child.firstChildElement(); !reasonChild.isNull();
                 reasonChild      = reasonChild.nextSiblingElement()) {
                if (reasonChild.namespaceURI() != JingleNs)
                    continue;
                const auto reasonName = localName(reasonChild);
                if (reasonName == QLatin1String("text"))
                    data->reasonText = reasonChild.text();
                else if (data->reasonCondition.isEmpty())
                    data->reasonCondition = reasonName;
            }
            continue;
        }

        if (name == QLatin1String("tie-break") && child.namespaceURI() == JmiNs) {
            data->tieBreak = true;
            continue;
        }

        if (name == QLatin1String("migrated") && child.namespaceURI() == JmiNs) {
            data->migratedTo = child.attribute(QStringLiteral("to"));
            continue;
        }

        const auto imported = data->extensionsDocument.importNode(child, true).toElement();
        if (!imported.isNull())
            data->extensions.append(imported);
    }

    return result.isValid() ? result : MessageInitiation();
}

bool MessageInitiation::isValid() const
{
    if (!d || d->action == Action::None || d->id.isEmpty())
        return false;
    if (d->action != Action::Propose)
        return true;
    if (d->descriptions.isEmpty())
        return false;
    for (const auto &description : d->descriptions) {
        if (description.isNull() || localName(description) != QLatin1String("description")
            || description.namespaceURI().isEmpty() || description.namespaceURI() == JmiNs)
            return false;
    }
    return true;
}

MessageInitiation::Action MessageInitiation::action() const { return d ? d->action : Action::None; }

QString MessageInitiation::id() const { return d ? d->id : QString(); }

QList<QDomElement> MessageInitiation::descriptions() const { return d ? d->descriptions : QList<QDomElement>(); }

void MessageInitiation::setDescriptions(const QList<QDomElement> &descriptions)
{
    auto data = ensureD();
    data->descriptions.clear();
    data->descriptionsDocument = QDomDocument();
    for (const auto &description : descriptions)
        addDescription(description);
}

void MessageInitiation::addDescription(const QDomElement &description)
{
    if (description.isNull())
        return;
    auto data     = ensureD();
    auto imported = data->descriptionsDocument.importNode(description, true).toElement();
    if (!imported.isNull())
        data->descriptions.append(imported);
}

void MessageInitiation::addDescription(const QString &applicationNamespace)
{
    if (applicationNamespace.isEmpty() || applicationNamespace == JmiNs)
        return;
    auto data        = ensureD();
    auto description = data->descriptionsDocument.createElementNS(applicationNamespace, QStringLiteral("description"));
    data->descriptions.append(description);
}

QString MessageInitiation::reasonCondition() const { return d ? d->reasonCondition : QString(); }

QString MessageInitiation::reasonText() const { return d ? d->reasonText : QString(); }

void MessageInitiation::setReason(const QString &condition, const QString &text)
{
    auto data             = ensureD();
    data->reasonCondition = condition;
    data->reasonText      = text;
}

bool MessageInitiation::tieBreak() const { return d && d->tieBreak; }

void MessageInitiation::setTieBreak(bool enabled) { ensureD()->tieBreak = enabled; }

QString MessageInitiation::migratedTo() const { return d ? d->migratedTo : QString(); }

void MessageInitiation::setMigratedTo(const QString &id) { ensureD()->migratedTo = id; }

QList<QDomElement> MessageInitiation::extensions() const { return d ? d->extensions : QList<QDomElement>(); }

void MessageInitiation::addExtension(const QDomElement &element)
{
    if (element.isNull())
        return;
    auto data     = ensureD();
    auto imported = data->extensionsDocument.importNode(element, true).toElement();
    if (!imported.isNull())
        data->extensions.append(imported);
}

QDomElement MessageInitiation::toXml(QDomDocument *doc) const
{
    if (!doc || !isValid())
        return {};

    const auto name = actionName(d->action);
    if (name.isEmpty())
        return {};

    auto element = doc->createElementNS(JmiNs, name);
    element.setAttribute(QStringLiteral("id"), d->id);

    if (d->action == Action::Propose) {
        for (const auto &description : d->descriptions)
            element.appendChild(doc->importNode(description, true));
    }

    if (!d->reasonCondition.isEmpty() || !d->reasonText.isEmpty()) {
        auto reason = doc->createElementNS(JingleNs, QStringLiteral("reason"));
        if (!d->reasonCondition.isEmpty())
            reason.appendChild(doc->createElementNS(JingleNs, d->reasonCondition));
        if (!d->reasonText.isEmpty()) {
            auto text = doc->createElementNS(JingleNs, QStringLiteral("text"));
            text.appendChild(doc->createTextNode(d->reasonText));
            reason.appendChild(text);
        }
        element.appendChild(reason);
    }

    if (d->tieBreak)
        element.appendChild(doc->createElementNS(JmiNs, QStringLiteral("tie-break")));

    if (!d->migratedTo.isEmpty()) {
        auto migrated = doc->createElementNS(JmiNs, QStringLiteral("migrated"));
        migrated.setAttribute(QStringLiteral("to"), d->migratedTo);
        element.appendChild(migrated);
    }

    for (const auto &extension : d->extensions)
        element.appendChild(doc->importNode(extension, true));

    return element;
}

}} // namespace XMPP::Jingle
