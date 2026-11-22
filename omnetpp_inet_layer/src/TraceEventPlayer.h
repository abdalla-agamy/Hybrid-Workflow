#ifndef DT_HSBP_TRACE_EVENT_PLAYER_H
#define DT_HSBP_TRACE_EVENT_PLAYER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef OMNETPP_STANDALONE_TEST
#include <omnetpp.h>
#include "DTHSBPPayload.h"
#include "inet/networklayer/common/L3Address.h"
#endif

namespace dthsbp {

struct TraceMeta {
    int seed = 42;
    int swarmSize = 0;
    int numClusters = 0;
    std::string protocol;
    double simulationTime = 0.0;
    double timeStep = 1.0;
    double arenaWidthM = 1000.0;
    double arenaHeightM = 1000.0;
    double maxVelocityMps = 20.0;
    int keySize = 32;
    int headerSize = 16;
    int controlSize = 8;
    int messageSize = 56;
    int packetSize = 256;
    int mtu = 1500;
};

struct NodeState {
    int uavId = 0;
    int clusterId = 0;
    std::string role;
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double energy = 100.0;
    bool active = true;
};

struct NodePositionSnapshot {
    int uavId = 0;
    int clusterId = 0;
    std::string role;
    double x = 0.0;
    double y = 0.0;
};

struct HopDescriptor {
    std::string fromRole;
    std::string toRole;
    int bytes = 0;
    std::string kind;
    int fanout = 0;
    int fragmentsCount = 0;
};

struct CryptoCost {
    double leaderTimeMs = 0.0;
    int rekeyCount = 0;
    int messagesSent = 0;
    int64_t bytesTransmitted = 0;
};

struct TraceEventRecord {
    double t = 0.0;
    std::string type;
    int clusterId = 0;
    std::vector<int> affectedUAVs;
    bool isCollateralBatch = false;
    bool hasPredictionHit = false;
    bool predictionHit = false;
    std::vector<NodePositionSnapshot> nodePositions;
    std::vector<HopDescriptor> hops;
    CryptoCost crypto;
    std::vector<int> clusterMembershipAfter;
};

struct PlayerSummary {
    std::string protocol;
    int swarmSize = 0;
    size_t numEvents = 0;
    size_t rekeyEvents = 0;
    size_t dtPredictedEvents = 0;
    size_t collateralBatches = 0;
    size_t collateralLeavesBatched = 0;
    int64_t totalCryptoBytes = 0;
    int64_t totalHopBytes = 0;
    int64_t totalMessagesSent = 0;
    int64_t totalFragments = 0;
    double totalLeaderTimeMs = 0.0;
    size_t activeUavsEnd = 0;
    size_t carryForwardNodesPreserved = 0;
    size_t maxBoundedSnapshotSize = 0;
    size_t chReassignments = 0;
};

/**
 * Loads deterministic MATLAB JSON traces and maintains synchronized full-swarm
 * 2D coordinates, cluster memberships, and CH/SL roles via bounded carry-forward.
 */
class TracePlayerCore {
public:
    void loadFromFile(const std::string& filePath);
    void resetState();
    void applyEvent(size_t eventIdx);
    void replayAll();

    const TraceMeta& meta() const { return meta_; }
    const std::vector<TraceEventRecord>& events() const { return events_; }
    const std::map<int, NodeState>& nodes() const { return nodes_; }
    const std::map<int, std::vector<int> >& clusterMembers() const { return clusterMembers_; }
    const std::map<int, int>& clusterHeads() const { return clusterHeads_; }
    int slId() const { return slId_; }
    const PlayerSummary& summary() const { return summary_; }

    double distanceBetween(int uavA, int uavB) const;
    int countActiveNodes() const;
    bool hasValidHeadForAllNonEmptyClusters() const;
    std::vector<int> activeUavIds() const;
    std::vector<int> clusterFollowers(int clusterId) const;
    int clusterHead(int clusterId) const;
    std::unordered_map<int, std::vector<int> > computeBfsPaths(
        int srcUavId,
        double commRangeM,
        const std::vector<int>& targets
    ) const;

private:
    TraceMeta meta_;
    std::vector<NodeState> initialNodes_;
    std::vector<TraceEventRecord> events_;

    std::map<int, NodeState> nodes_;
    std::map<int, std::vector<int> > clusterMembers_;
    std::map<int, int> clusterHeads_;
    int slId_ = -1;
    PlayerSummary summary_;
};

#ifndef OMNETPP_STANDALONE_TEST

class TraceMobility;
class DTHSBPRekeyApp;

/**
 * OMNeT++ cSimpleModule driving bounded INET mobility updates and packet-level
 * UDP/IEEE 802.11 DCF transmissions for every MATLAB trace event.
 */
class TraceEventPlayer : public omnetpp::cSimpleModule {
public:
    struct ActiveTx {
        int txId = 0;
        bool isBroadcast = false;
        int srcUavId = -1;
        int payloadBytes = 0;
        int numFrags = 1;
        int attempt = 0;
        int flatPathIdx = -1; // >=0 for FlatSBP path forwarding
        size_t fullPathPos = 0;
        std::vector<int> fullPath;
        std::vector<int> expectedInRange;
        std::unordered_map<int, std::vector<bool> > fragAcked;
        std::unordered_set<int> completedUavs;
        bool finished = false;
    };

    struct EventPerfRecord {
        double t = 0.0;
        std::string eventType;
        int clusterId = 0;
        int rekeyCount = 0;
        bool isCollateralBatch = false;
        int affectedCount = 0;
        int hasPredictionHit = 0;
        int predictionHit = 0;
        int64_t cryptoBytes = 0;
        int64_t bytesOnAir = 0;
        int64_t fragmentsSent = 0;
        int64_t retransmissions = 0;
        double cryptoLatencyMs = 0.0;
        double networkLatencyMs = 0.0;
        double totalLatencyMs = 0.0;
        double pdr = 1.0;
        double keySyncRatio = 1.0;
    };

    TraceEventPlayer() = default;
    virtual ~TraceEventPlayer();

    void onPacketArrived(int hostIndex, int uavId, const WireRekeyHeader& hdr, int packetBytes);

protected:
    virtual int numInitStages() const override { return inet::NUM_INIT_STAGES; }
    virtual void initialize(int stage) override;
    virtual void handleMessage(omnetpp::cMessage* msg) override;
    virtual void finish() override;

private:
    void initHostMappings();
    void applyHostSlotUpdatesForEvent(const TraceEventRecord& ev);
    void moveSnapshotHosts(const TraceEventRecord& ev);
    void beginEvent(size_t evIdx);
    void beginNetworkPhase();
    void startNextHopInEvent();
    void launchBroadcastDirectWave();
    void launchBroadcastRelayWave();
    void scheduleStaggeredRelayDispatch(size_t pos, double delaySec);
    void clearPendingRelayTimers();
    void dispatchTx(ActiveTx& tx, const std::vector<int>& receivers);
    void armStageTimer(int totalPayloadBitsEstimate, int txCount);
    void checkCurrentStageCompletion();
    void handleStageTimeout();
    void completeCurrentEvent();
    bool dropByExtraChannelLoss(int srcUavId, int dstUavId);

    TracePlayerCore core_;

    // Parameters
    std::string traceFile_;
    std::string eventLogFile_;
    bool enableMultiHop_ = true;
    int mtuBytes_ = 1500;
    int maxRetries_ = 3;
    double commRangeM_ = 350.0;
    double carrierSenseRangeM_ = 700.0;
    double bitrateMbps_ = 6.0;
    double pLossExtra_ = 0.0;
    double retryTimeoutMultiplier_ = 3.0;
    double relayJitterMaxMs_ = 2.0;

    // Host slots
    int numUavs_ = 0;
    std::vector<TraceMobility*> mobilities_;
    std::vector<DTHSBPRekeyApp*> apps_;
    std::unordered_map<int, int> uavToSlot_;
    std::vector<int> slotToUav_;
    std::deque<int> freeSlots_;

    // Timers
    omnetpp::cMessage* stepTimer_ = nullptr;
    omnetpp::cMessage* netStartTimer_ = nullptr;
    omnetpp::cMessage* stageTimeoutTimer_ = nullptr;
    std::unordered_map<omnetpp::cMessage*, std::pair<int, size_t> > pendingRelayTimers_;

    // Event & stage state machine
    size_t curEvIdx_ = 0;
    bool isFlatSbpEvent_ = false;
    bool slToChOk_ = true;
    omnetpp::simtime_t evNetStartTime_;
    int nextTxId_ = 1;

    // Hop queue within event (for HSBP/DTHSBP/LKH/TGDH)
    size_t curHopIdx_ = 0;

    // Broadcast dissemination state for current CH_TO_FOLLOWER hop
    enum class BcastSubphase { None, DirectWave, RelayWave };
    BcastSubphase bcastSubphase_ = BcastSubphase::None;
    int bcastChId_ = -1;
    int bcastBytes_ = 0;
    std::vector<int> bcastFollowers_;
    std::unordered_map<int, size_t> bcastFollowerIndex_;
    std::unordered_set<int> bcastCoveredFrontier_;
    std::unordered_map<int, std::vector<int> > bcastRelayPaths_;
    std::vector<bool> bcastDelivered_;
    int bcastCurrentDepth_ = 1;
    int bcastMaxDepth_ = 0;

    // Active transactions inside current wave/hop
    std::vector<ActiveTx> activeTxs_;
    std::unordered_map<int, size_t> txIdToPos_;
    std::vector<bool> flatTargetOk_;
    std::vector<double> currentHopPdrs_;
    std::vector<std::vector<bool> > currentRoundSyncMasks_;

    // Per-event accumulators
    int64_t evBytesOnAir_ = 0;
    int64_t evFragsSent_ = 0;
    int64_t evRetx_ = 0;

    // Channel loss state (Gilbert-Elliott bursty model layered atop INET PHY/MAC)
    std::mt19937 lossRng_;
    std::unordered_map<uint64_t, bool> geBadState_;

    // Signals & run-level records
    omnetpp::simsignal_t rekeyLatencySig_ = -1;
    omnetpp::simsignal_t eventPdrSig_ = -1;
    omnetpp::simsignal_t eventKeySyncSig_ = -1;
    std::vector<EventPerfRecord> perfRecords_;
};

#endif // OMNETPP_STANDALONE_TEST

} // namespace dthsbp

#endif // DT_HSBP_TRACE_EVENT_PLAYER_H
