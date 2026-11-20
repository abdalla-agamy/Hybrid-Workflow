#ifndef DT_HSBP_REKEY_APP_H
#define DT_HSBP_REKEY_APP_H

#include "DTHSBPPayload.h"
#include "inet/applications/base/ApplicationBase.h"
#include "inet/networklayer/common/L3Address.h"
#include "inet/transportlayer/contract/udp/UdpSocket.h"

namespace dthsbp {

class TraceEventPlayer;

/**
 * Per-UAV UDP application sending/receiving MTU-bounded key-management fragments
 * across INET's real IPv4 + IEEE 802.11 DCF MAC/PHY stack.
 */
class DTHSBPRekeyApp : public inet::ApplicationBase, public inet::UdpSocket::ICallback {
public:
    DTHSBPRekeyApp() = default;
    virtual ~DTHSBPRekeyApp() = default;

    void setAssignedUavId(int uavId, bool active);
    int assignedUavId() const { return assignedUavId_; }
    bool isUavActive() const { return uavActive_; }
    int hostIndex() const { return hostIndex_; }
    inet::L3Address localAddress();

    /** Split payloadBytes across mtuBytes fragments and transmit via UdpSocket. */
    void sendRekeyFragments(
        const inet::L3Address& destAddr,
        int destPort,
        WireRekeyHeader hdrTemplate,
        int payloadBytes,
        int mtuBytes
    );

protected:
    virtual int numInitStages() const override { return inet::NUM_INIT_STAGES; }
    virtual void initialize(int stage) override;
    virtual void handleMessageWhenUp(omnetpp::cMessage* msg) override;
    virtual void finish() override;

    virtual void handleStartOperation(inet::LifecycleOperation* operation) override;
    virtual void handleStopOperation(inet::LifecycleOperation* operation) override;
    virtual void handleCrashOperation(inet::LifecycleOperation* operation) override;

    virtual void socketDataArrived(inet::UdpSocket* socket, inet::Packet* packet) override;
    virtual void socketErrorArrived(inet::UdpSocket* socket, inet::Indication* indication) override;
    virtual void socketClosed(inet::UdpSocket* socket) override;

private:
    int localPort_ = 5000;
    int hostIndex_ = -1;
    int assignedUavId_ = -1;
    int wlanInterfaceId_ = -1;
    bool uavActive_ = false;
    int64_t numSent_ = 0;
    int64_t numReceived_ = 0;

    inet::UdpSocket socket_;
    inet::L3Address cachedLocalAddr_;
    TraceEventPlayer* player_ = nullptr;
};

} // namespace dthsbp

#endif // DT_HSBP_REKEY_APP_H
