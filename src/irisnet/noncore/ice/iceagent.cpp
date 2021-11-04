#include "iceagent.h"

#include <QCoreApplication>
#include <QtCrypto>

namespace XMPP::ICE {

struct Foundation {
    CandidateType               type;
    const QHostAddress          baseAddr;
    const QHostAddress          stunServAddr;
    QAbstractSocket::SocketType stunRequestProto;

    bool operator==(const Foundation &f) const
    {
        return type == f.type && baseAddr == f.baseAddr && stunServAddr == f.stunServAddr
            && stunRequestProto == f.stunRequestProto;
    };
};

inline uint qHash(const Foundation &f, uint seed = 0)
{
    auto tmp = uint(f.stunRequestProto) & (uint(f.type) << 8);
    return qHash(f.baseAddr, seed) ^ qHash(f.stunServAddr, seed) ^ tmp;
}

static QChar randomPrintableChar()
{
    // 0-25 = a-z
    // 26-51 = A-Z
    // 52-61 = 0-9

    uchar c = static_cast<uchar>(QCA::Random::randomChar() % 62);
    if (c <= 25)
        return 'a' + c;
    else if (c <= 51)
        return 'A' + (c - 26);
    else
        return '0' + (c - 52);
}

struct Agent::Private {
    QHash<Foundation, QString> foundations;
};

Agent *Agent::instance()
{
    static auto i = new Agent(QCoreApplication::instance());
    return i;
}

Agent::~Agent() { }

QString Agent::foundation(CandidateType type, const QHostAddress baseAddr, const QHostAddress &stunServAddr,
                          QAbstractSocket::SocketType stunRequestProto)
{
    Foundation f { type, baseAddr, stunServAddr, stunRequestProto };
    QString    ret = d->foundations.value(f);
    if (ret.isEmpty()) {
        do {
            ret = randomCredential(8);
        } while (std::find_if(d->foundations.begin(), d->foundations.end(), [&](auto const &fp) { return fp == ret; })
                 != d->foundations.end());
        d->foundations.insert(f, ret);
    }
    return ret;
}

QString Agent::randomCredential(int len)
{
    QString out;
    out.reserve(len);
    for (int n = 0; n < len; ++n)
        out += randomPrintableChar();
    return out;
}

Agent::Agent(QObject *parent) : QObject(parent), d(new Private) { }

} // namespace XMPP
