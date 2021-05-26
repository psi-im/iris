#include "xmpp_externalservicediscovery.h"

#include "xmpp_client.h"
#include "xmpp_jid.h"
#include "xmpp_xmlcommon.h"

namespace XMPP {

JT_ExternalServiceDiscovery::JT_ExternalServiceDiscovery(Task *parent) : Task(parent) { }

void JT_ExternalServiceDiscovery::getServices(const QString &type)
{
    type_ = type;
    credHost_.clear(); // to indicate it's services request, not creds
}

void JT_ExternalServiceDiscovery::getCredentials(const QString &host, const QString &type, uint16_t port)
{
    Q_ASSERT(!host.isEmpty());
    Q_ASSERT(!type.isEmpty());
    credHost_ = host;
    type_     = type;
    credPort_ = port;
}

void JT_ExternalServiceDiscovery::onGo()
{
    QDomElement iq    = createIQ(doc(), "get", client()->jid().domain(), id());
    QDomElement query = doc()->createElementNS(QLatin1String("urn:xmpp:extdisco:2"),
                                               QLatin1String(credHost_.isEmpty() ? "services" : "credentials"));
    if (credHost_.isEmpty()) {
        if (!type_.isEmpty()) {
            query.setAttribute(QLatin1String("type"), type_);
        }
    } else {
        QDomElement service = doc()->createElement(QLatin1String("service"));
        service.setAttribute(QLatin1String("host"), credHost_);
        service.setAttribute(QLatin1String("type"), type_);
        query.appendChild(service);
    }
    iq.appendChild(query);
    send(iq);
}

bool JT_ExternalServiceDiscovery::take(const QDomElement &x)
{
    if (!iqVerify(x, Jid(QString(), client()->jid().domain()), id()))
        return false;

    if (x.attribute("type") == "result") {
        auto query = x.firstChildElement(QLatin1String(credHost_.isEmpty() ? "services" : "credentials"));
        if (query.namespaceURI() != QLatin1String("urn:xmpp:extdisco:2")) {
            setError(0, QLatin1String("invalid namespace"));
            return true;
        }
        QString serviceTag(QLatin1String("service"));
        for (auto el = query.firstChildElement(serviceTag); !el.isNull(); el = el.nextSiblingElement(serviceTag)) {
            services_.append(Service {});
            auto &s = services_.last();
            if (!s.parse(el)) {
                services_.removeLast();
            }
        }
        setSuccess();
    } else {
        setError(x);
    }

    return true;
}

bool JT_ExternalServiceDiscovery::Service::parse(QDomElement &el)
{
    QString actionOpt     = el.attribute(QLatin1String("action"));
    QString expiresOpt    = el.attribute(QLatin1String("expires"));
    name                  = el.attribute(QLatin1String("name"));
    password              = el.attribute(QLatin1String("password"));
    QString restrictedOpt = el.attribute(QLatin1String("restricted"));
    transport             = el.attribute(QLatin1String("transport"));
    username              = el.attribute(QLatin1String("username"));
    host                  = el.attribute(QLatin1String("host"));
    QString portReq       = el.attribute(QLatin1String("port"));
    type                  = el.attribute(QLatin1String("type"));

    bool ok;
    if (host.isEmpty() || portReq.isEmpty() || type.isEmpty())
        return false;

    port = portReq.toUShort(&ok);
    if (!ok)
        return false;

    if (!expiresOpt.isEmpty()) {
        expires = QDateTime::fromString(el.text().left(19), Qt::ISODate);
        if (!expires.isValid())
            return false;
    }

    if (actionOpt.isEmpty()) {
        if (actionOpt == QLatin1String("add"))
            action = Action::Add;
        else if (actionOpt == QLatin1String("modify"))
            action = Action::Modify;
        else if (actionOpt == QLatin1String("delete"))
            action = Action::Delete;
        else
            return false;
    }

    if (!restrictedOpt.isEmpty()) {
        if (restrictedOpt == QLatin1String("true"))
            restricted = true;
        if (restrictedOpt != QLatin1String("false"))
            return false;
    }

    return true;
}

} // namespace XMPP
