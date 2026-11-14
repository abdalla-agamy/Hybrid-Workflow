#ifndef DT_HSBP_TRACE_EVENT_PLAYER_H
#define DT_HSBP_TRACE_EVENT_PLAYER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#ifndef OMNETPP_STANDALONE_TEST
#include <omnetpp.h>
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
/**
 * OMNeT++ cSimpleModule wrapper executing TracePlayerCore step-by-step at event
 * timestamps ev.t inside INET 4.5 IEEE 802.11 simulations.
 */
class TraceEventPlayer : public omnetpp::cSimpleModule {
public:
    TraceEventPlayer() : stepTimer_(nullptr), nextEventIdx_(0) {}
    virtual ~TraceEventPlayer();

protected:
    virtual void initialize() override;
    virtual void handleMessage(omnetpp::cMessage* msg) override;
    virtual void finish() override;

private:
    void scheduleNextTraceEvent();

    TracePlayerCore core_;
    omnetpp::cMessage* stepTimer_;
    size_t nextEventIdx_;
};
#endif // OMNETPP_STANDALONE_TEST

} // namespace dthsbp

#endif // DT_HSBP_TRACE_EVENT_PLAYER_H
