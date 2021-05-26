#ifndef XMPP_EXTERNALSERVICEDISCOVERY_H
#define XMPP_EXTERNALSERVICEDISCOVERY_H

#include "xmpp_task.h"

#include <QDateTime>
#include <QObject>

namespace XMPP {

// XEP-0215 0.7
class JT_ExternalServiceDiscovery : public Task {
    Q_OBJECT
public:
    enum class Action { Add, Delete, Modify };

    struct Service {
        Action    action = Action::Add;
        QDateTime expires;            // optional
        QString   host;               // required
        QString   name;               // optional
        QString   password;           // optional
        QString   port;               // required
        bool      restricted = false; // optional
        QString   transport;          // optional
        QString   type;               // required
        QString   username;           // optional

        bool parse(QDomElement &el);
    };

    explicit JT_ExternalServiceDiscovery(Task *parent = nullptr);

    void                           getServices(const QString &type = QString());
    void                           getCredentials(const QString &host, const QString &type, std::uint16_t port = 0);
    inline const QVector<Service> &services() const { return services_; }

    void onGo();
    bool take(const QDomElement &);

private:
    std::uint16_t credPort_ = 0;
    QString       credHost_;
    QString       type_;

    QVector<Service> services_; // result
};

} // namespace XMPP

#endif // XMPP_EXTERNALSERVICEDISCOVERY_H
