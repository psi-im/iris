#ifndef XMPP_ICEAGENT_H
#define XMPP_ICEAGENT_H

#include "icecandidate.h"
#include "icecomponent.h"

#include <QObject>
#include <memory>

namespace XMPP::ICE {

class Agent : public QObject {
    Q_OBJECT
public:
    static Agent *instance();
    ~Agent();

    QString foundation(CandidateType type, const QHostAddress baseAddr,
                       const QHostAddress &        stunServAddr     = QHostAddress(),
                       QAbstractSocket::SocketType stunRequestProto = QAbstractSocket::UnknownSocketType);

    static QString randomCredential(int len);

private:
    explicit Agent(QObject *parent = nullptr);

signals:

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace XMPP

#endif // XMPP_ICEAGENT_H
