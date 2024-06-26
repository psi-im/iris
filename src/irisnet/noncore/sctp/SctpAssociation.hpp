#ifndef MS_RTC_SCTP_ASSOCIATION_HPP
#define MS_RTC_SCTP_ASSOCIATION_HPP

// #include "common.hpp"
// #include "Utils.hpp"
#include "RTC/DataConsumer.hpp"
#include "RTC/DataProducer.hpp"
#include "usrsctp.h"
#include <QJsonObject>
#include <QObject>
#include <QtEndian>
#include <functional>

// using json = nlohmann::json;

namespace RTC {
class SctpAssociation {
public:
    enum class SctpState { NEW = 1, CONNECTING, CONNECTED, FAILED, CLOSED };

private:
    enum class StreamDirection { INCOMING = 1, OUTGOING };

protected:
    using onQueuedCallback = const std::function<void(bool queued)>;

public:
    class Listener {
    public:
        virtual void OnSctpAssociationConnecting(RTC::SctpAssociation *sctpAssociation) = 0;
        virtual void OnSctpAssociationConnected(RTC::SctpAssociation *sctpAssociation)  = 0;
        virtual void OnSctpAssociationFailed(RTC::SctpAssociation *sctpAssociation)     = 0;
        virtual void OnSctpAssociationClosed(RTC::SctpAssociation *sctpAssociation)     = 0;
        virtual void OnSctpAssociationSendData(RTC::SctpAssociation *sctpAssociation, const uint8_t *data, size_t len)
            = 0;
        virtual void OnSctpAssociationMessageReceived(RTC::SctpAssociation *sctpAssociation, uint16_t streamId,
                                                      uint32_t ppid, const uint8_t *msg, size_t len)
            = 0;
        virtual void OnSctpAssociationBufferedAmount(RTC::SctpAssociation *sctpAssociation, size_t len) = 0;
        virtual void OnSctpStreamClosed(RTC::SctpAssociation *sctpAssociation, uint16_t streamId)         = 0;
    };

public:
    static bool IsSctp(const uint8_t *data, size_t len)
    {
        return ((len >= 12) &&
                // Must have Source Port Number and Destination Port Number set to 5000 (hack).
                (qFromBigEndian<quint16>(data) == 5000) && (qFromBigEndian<quint16>(data + 2) == 5000));
    }

public:
    SctpAssociation(Listener *listener, uint16_t os, uint16_t mis, size_t maxSctpMessageSize, size_t sctpSendBufferSize,
                    bool isDataChannel);
    ~SctpAssociation();

public:
    void      FillJson(QJsonObject &jsonObject) const;
    void      TransportConnected();
    SctpState GetState() const { return this->state; }
    size_t    GetSctpBufferedAmount() const { return this->sctpBufferedAmount; }
    void      ProcessSctpData(const uint8_t *data, size_t len);
    void      SendSctpMessage(RTC::DataConsumer *dataConsumer, uint32_t ppid, const uint8_t *msg, size_t len,
                              onQueuedCallback *cb = nullptr);
    void      HandleDataConsumer(RTC::DataConsumer *dataConsumer);
    void      DataProducerClosed(RTC::DataProducer *dataProducer);
    void      DataConsumerClosed(RTC::DataConsumer *dataConsumer);
    bool      isSendBufferFull() const { return sendBufferFull; }

private:
    void ResetSctpStream(uint16_t streamId, StreamDirection);
    void AddOutgoingStreams(bool force = false);

    /* Callbacks fired by usrsctp events. */
public:
    void OnUsrSctpSendSctpData(void *buffer, size_t len);
    void OnUsrSctpReceiveSctpData(uint16_t streamId, uint16_t ssn, uint32_t ppid, int flags, const uint8_t *data,
                                  size_t len);
    void OnUsrSctpReceiveSctpNotification(union sctp_notification *notification, size_t len);
    void OnUsrSctpSentData(uint32_t freeBuffer);

public:
    uintptr_t id { 0u };

private:
    // Passed by argument.
    Listener *listener { nullptr };
    uint16_t  os { 1024u };
    uint16_t  mis { 1024u };
    size_t    maxSctpMessageSize { 262144u };
    size_t    sctpSendBufferSize { 262144u };
    size_t    sctpBufferedAmount { 0u };
    bool      isDataChannel { false };
    bool      sendBufferFull { false };
    // Allocated by this.
    uint8_t *messageBuffer { nullptr };
    // Others.
    SctpState      state { SctpState::NEW };
    struct socket *socket { nullptr };
    uint16_t       desiredOs { 0u };
    size_t         messageBufferLen { 0u };
    uint16_t       lastSsnReceived { 0u }; // Valid for us since no SCTP I-DATA support.
};
} // namespace RTC

#endif
