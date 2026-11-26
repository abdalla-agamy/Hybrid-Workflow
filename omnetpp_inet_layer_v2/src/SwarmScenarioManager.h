#ifndef DT_HSBP_V2_SWARM_SCENARIO_MANAGER_H
#define DT_HSBP_V2_SWARM_SCENARIO_MANAGER_H

#include "DTLogic.h"
#include "V2Payloads.h"

#include <omnetpp.h>
#include "inet/networklayer/common/L3Address.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dthsbp_v2 {

class SwarmMobility;
class DTHSBPNodeApp;

class SwarmScenarioManager : public omnetpp::cSimpleModule {
public:
    virtual ~SwarmScenarioManager() override;

    // Static configuration queries (mission parameters, not dynamic event oracles)
    ProtoKind getProtoKind() const { return protoKind_; }
    int getNumClusters() const { return numClusters_; }
    int getAppPort() const { return appPort_; }
    int getMtuBytes() const { return mtuBytes_; }
    double getDtReportIntervalSec() const { return dtReportIntervalSec_; }
    double getChSummaryIntervalSec() const { return chSummaryIntervalSec_; }
    double getC2BroadcastIntervalSec() const { return c2BroadcastIntervalSec_; }
    double getMissedHeartbeatTimeoutSec() const { return missedHeartbeatTimeoutSec_; }
    double getThetaLeave() const { return thetaLeave_; }
    double getThetaJoin() const { return thetaJoin_; }
    double getCollateralLookaheadSec() const { return collateralLookaheadSec_; }
    double getRelayJitterMaxMs() const { return relayJitterMaxMs_; }
    int getTotalSlots() const { return totalSlots_; }
    inet::L3Address getSlotAddress(int slotIdx) const;

    // Passive instrumentation hooks called by DTHSBPNodeApp (never trigger protocol logic)
    void recordPacketTx(MsgKind kind, int bytes, bool isAck, bool isResync);
    void recordRekeyRound(
        bool isDtProactive,
        int excludedNodeId,
        int collateralCount,
        bool isCollateralBatch,
        double cryptoLatencyMs,
        double completionLatencyMs,
        double deliveryRatio
    );
    void recordNodeExcludedByProtocol(int uavSlotId, double simTimeSec);
    void recordC2Sent(uint64_t requiredKeyEpoch, double simTimeSec);
    void recordC2Delivery(bool decryptedOk, double endToEndLatencyMs);
    void recordDesyncDurationMs(double durationMs);
    void recordSlFailoverCompleted();
    void recordRelaySuppressed();

    // Cluster topology directory helper (pre-mission cluster centroid table)
    std::vector<int> getInitialClusterMembers(int clusterId) const;
    int getInitialClusterHead(int clusterId) const;
    int getInitialSlSlot() const { return 0; }
    int getActiveSwarmCountEstimate() const { return activePhysicalCount_; }

protected:
    virtual int numInitStages() const override { return 4; }
    virtual void initialize(int stage) override;
    virtual void handleMessage(omnetpp::cMessage* msg) override;
    virtual void finish() override;

private:
    enum class ScheduledAction : uint8_t {
        INIT_BOOT = 0,
        PRECURSOR_LEAVE = 1,
        PHYSICAL_LEAVE = 2,
        SUDDEN_CRASH = 3,
        DISTURBANCE = 4,
        PHYSICAL_JOIN = 5,
        SL_FAILOVER = 6
    };

    struct ScheduledEvent {
        double timeSec = 0.0;
        ScheduledAction action = ScheduledAction::PHYSICAL_JOIN;
        int slotIdx = -1;
        int clusterId = 0;
        int targetNodeId = -1; // slotIdx when referring to slot
    };

    struct AnomalyAuditRecord {
        bool anomalyActive = false;
        bool excludedByProtocol = false;
        double startTimeSec = 0.0;
        double excludedTimeSec = -1.0;
        int exposurePackets = 0;
    };

    void schedulePhysicalTimeline();
    void executeBootstrap();
    void executePrecursor(int slotIdx);
    void executePhysicalLeave(int slotIdx);
    void executeSuddenCrash(int slotIdx);
    void executeDisturbance(int slotIdx);
    void executePhysicalJoin(int clusterId);
    void executeSlFailover();

    double clusterCenterX(int clusterId) const;
    double clusterCenterY(int clusterId) const;

    ProtoKind protoKind_ = ProtoKind::DTHSBP;
    int initialSwarmSize_ = 50;
    int numClusters_ = 5;
    int totalSlots_ = 70;
    int appPort_ = 5000;
    int mtuBytes_ = 1500;
    bool qosEnabled_ = true;
    double ricianKDb_ = 4.0;
    double simDurationSec_ = 45.0;
    double joinRatePerSec_ = 0.25;
    double leaveRatePerSec_ = 0.20;
    double failureRatePerSec_ = 0.10;
    double disturbanceRatePerSec_ = 0.10;
    double slFailoverTimeSec_ = 25.0;
    double precursorLeadSec_ = 2.3;
    double dtReportIntervalSec_ = 1.0;
    double chSummaryIntervalSec_ = 5.0;
    double c2BroadcastIntervalSec_ = 0.2;
    double missedHeartbeatTimeoutSec_ = 3.0;
    double thetaLeave_ = 2.5;
    double thetaJoin_ = 1.0;
    double collateralLookaheadSec_ = 1.5;
    double relayJitterMaxMs_ = 2.0;
    double playgroundSizeX_ = 1000.0;
    double playgroundSizeY_ = 1000.0;
    double chRingRadiusM_ = 140.0;
    double clusterRadiusM_ = 100.0;

    std::vector<SwarmMobility*> mobilities_;
    std::vector<DTHSBPNodeApp*> apps_;
    mutable std::vector<inet::L3Address> slotAddrs_;
    std::vector<bool> slotOccupied_;
    std::vector<int> slotCluster_;
    std::vector<int> clusterHeadSlot_;
    std::unordered_map<int, AnomalyAuditRecord> anomalyAudit_;
    std::vector<ScheduledEvent> timeline_;
    std::vector<omnetpp::cMessage*> pendingTimers_;

    int activePhysicalCount_ = 0;
    bool initializedSwarm_ = false;

    // Aggregated telemetry counters
    double rekeyBytesOnAir_ = 0.0;
    double controlDtBytesOnAir_ = 0.0;
    double c2BytesOnAir_ = 0.0;
    long rekeyFragmentsSent_ = 0;
    long rekeyAcksSent_ = 0;
    long keyResyncPullsSent_ = 0;
    long relaysSuppressed_ = 0;

    long rekeyRounds_ = 0;
    long dtProactiveRounds_ = 0;
    long collateralBatches_ = 0;
    long collateralLeavesBatched_ = 0;
    long reactiveRounds_ = 0;
    long slFailoverCompleted_ = 0;

    std::vector<double> cryptoLatenciesMs_;
    std::vector<double> rekeyCompletionLatenciesMs_;
    std::vector<double> rekeyPdrSamples_;
    std::vector<double> c2LatenciesMs_;
    std::vector<double> desyncDurationsMs_;

    long c2BcastsSent_ = 0;
    long c2ExpectedReceptions_ = 0;
    long c2DecryptedOk_ = 0;
};

} // namespace dthsbp_v2

#endif // DT_HSBP_V2_SWARM_SCENARIO_MANAGER_H
