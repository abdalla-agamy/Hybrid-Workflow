#ifndef DT_HSBP_V2_NODE_APP_H
#define DT_HSBP_V2_NODE_APP_H

#include "DTLogic.h"
#include "V2Payloads.h"

#include "inet/applications/base/ApplicationBase.h"
#include "inet/transportlayer/contract/udp/UdpSocket.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dthsbp_v2 {

class SwarmMobility;
class SwarmScenarioManager;

class DTHSBPNodeApp : public inet::ApplicationBase, public inet::UdpSocket::ICallback {
public:
    virtual ~DTHSBPNodeApp() override;

    // Lifecycle activation from SwarmScenarioManager (ground-truth spawn / crash / park)
    void activateNode(
        int slotIdx,
        NodeRole role,
        int clusterId,
        int chSlotIdx,
        int slSlotIdx,
        uint64_t initialKeyEpoch
    );
    void deactivateNode();
    void setSimulatedLinkQuality(double linkQ);

    int getSlotIdx() const { return slotIdx_; }
    NodeRole getRole() const { return role_; }
    uint64_t getCurrentKeyEpoch() const { return clusterKeyEpoch_; }

protected:
    virtual int numInitStages() const override { return inet::NUM_INIT_STAGES; }
    virtual void initialize(int stage) override;
    virtual void handleMessageWhenUp(inet::cMessage* msg) override;
    virtual void handleStartOperation(inet::LifecycleOperation* operation) override;
    virtual void handleStopOperation(inet::LifecycleOperation* operation) override;
    virtual void handleCrashOperation(inet::LifecycleOperation* operation) override;

    virtual void socketDataArrived(inet::UdpSocket* socket, inet::Packet* packet) override;
    virtual void socketErrorArrived(inet::UdpSocket* socket, inet::Indication* indication) override;
    virtual void socketClosed(inet::UdpSocket* socket) override;

private:
    enum TimerKind : int {
        TIMER_DT_REPORT = 1001,
        TIMER_CH_SUMMARY = 1002,
        TIMER_C2_BCAST = 1003,
        TIMER_WATCHDOG = 1004,
        TIMER_RELAY_FWD = 1005,
        TIMER_ACK_TX = 1006,
        TIMER_CRYPTO_DONE = 1007,
        TIMER_REKEY_DEADLINE = 1008
    };

    struct PendingRekeyRound {
        uint32_t roundId = 0;
        uint64_t targetEpoch = 0;
        double startTimeSec = 0.0;
        double cryptoDelayMs = 0.0;
        bool isDtProactive = false;
        bool isCollateralBatch = false;
        bool isSlPromotion = false;
        bool isJoinAdmission = false;
        int targetClusterId = -1; // -1 = swarm-wide
        int payloadBytes = 48;
        int expectedResponders = 1;
        std::unordered_set<int> respondersReceived;
        std::vector<int> excludedSlots;
        bool finalized = false;
        inet::cMessage* deadlineMsg = nullptr;
    };

    struct PendingFragAssembly {
        int numFrags = 1;
        std::unordered_set<int> fragsSeen;
    };

    // Sending primitives with UserPriorityReq EDCA tagging
    void sendRawPacket(
        const char* name,
        MsgKind kind,
        const void* data,
        int lenBytes,
        const inet::L3Address& destAddr
    );
    void dispatchRekeyBroadcast(
        uint32_t roundId,
        uint64_t targetEpoch,
        int targetClusterId,
        int targetExcludedId,
        int payloadBytes,
        uint16_t flags,
        uint8_t hopStage,
        uint8_t ttl
    );

    // Protocol actions
    void sendDtReport();
    void sendChSummary();
    void sendC2Broadcast();
    void runWatchdog();
    void triggerRekeyForTarget(
        int clusterScopeId,
        const std::vector<int>& excludedIds,
        bool isDtProactive,
        bool isCollateralBatch,
        bool isSlPromotion,
        bool isJoinAdmission = false
    );
    void sendChEscalationOrEpochAck(
        int destSlSlot,
        int targetClusterId,
        int subjectNodeId,
        uint64_t epochVal,
        bool isEscalation,
        bool isJoinAdmission
    );
    void launchRekeyAfterCryptoDelay(uint32_t roundId);
    void finalizeRekeyRound(uint32_t roundId);
    void scheduleAckReply(int destSlotIdx, uint32_t roundId, uint64_t epoch);
    void scheduleRelayForward(const V2WireHeader& hdr, const std::vector<uint8_t>& rawBytes);

    // Incoming packet handlers
    void handleHeartbeatDtReport(const HeartbeatDtPacket& pkt);
    void handleChSummaryToSl(const ChSummaryPacket& pkt);
    void handleMissionC2(const C2BroadcastPacket& pkt, const std::vector<uint8_t>& rawBytes);
    void handleRekeyBroadcast(const V2WireHeader& hdr, const std::vector<uint8_t>& rawBytes);
    void handleRekeyAck(const V2WireHeader& hdr);
    void handleKeyResyncPull(const KeyResyncPacket& pkt);

    void clearAllTimers();
    void ensureWlanInterfaceId();
    uint32_t nextSeq() { return ++localSeqCounter_; }

    inet::UdpSocket socket_;
    int localPort_ = 5000;
    int wlanInterfaceId_ = -1;
    int slotIdx_ = -1;
    int relaySuppressionThreshold_ = 2;
    double ackDeadlineSec_ = 0.45;
    double resyncCooldownSec_ = 0.40;

    SwarmScenarioManager* manager_ = nullptr;
    SwarmMobility* mobility_ = nullptr;

    // Runtime closed-loop node state
    bool active_ = false;
    bool needsJoinAdmission_ = false;
    NodeRole role_ = NodeRole::FOLLOWER;
    int clusterId_ = -1;
    int chSlotIdx_ = -1;
    int slSlotIdx_ = 0;
    uint64_t swarmKeyEpoch_ = 0;
    uint64_t clusterKeyEpoch_ = 0;
    uint32_t slClusterEpochs_[5] = {1, 1, 1, 1, 1};
    double simulatedLinkQ_ = 0.96;
    double desyncedSinceSec_ = -1.0;
    double lastResyncPullSec_ = -10.0;
    double lastC2OrSlHeardSec_ = 0.0;
    bool slFailoverTriggered_ = false;
    uint32_t localSeqCounter_ = 0;
    uint32_t localRoundCounter_ = 0;

    // CH/SL DTServer & membership state
    DTKalmanTracker dtTracker_;
    std::unordered_set<int> clusterMembers_;
    std::unordered_set<int> excludedSlots_;
    std::unordered_set<int> activeClusters_;
    std::unordered_map<uint32_t, PendingRekeyRound> pendingRounds_;
    std::unordered_map<uint32_t, PendingFragAssembly> rxFragAssemblies_;

    // Decentralized relay dedup & counter-suppression tracking
    SeenPacketCache seenCache_{4096};
    std::unordered_map<uint64_t, int> relayOverhearCount_;
    std::unordered_map<uint64_t, inet::cMessage*> pendingRelayTimers_;
    std::vector<inet::cMessage*> transientTimers_;

    // Persistent periodic timers
    inet::cMessage* dtReportTimer_ = nullptr;
    inet::cMessage* chSummaryTimer_ = nullptr;
    inet::cMessage* c2Timer_ = nullptr;
    inet::cMessage* watchdogTimer_ = nullptr;
};

} // namespace dthsbp_v2

#endif // DT_HSBP_V2_NODE_APP_H
