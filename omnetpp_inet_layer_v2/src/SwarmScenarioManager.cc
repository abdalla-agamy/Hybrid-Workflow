#include "SwarmScenarioManager.h"
#include "SwarmMobility.h"
#include "DTHSBPNodeApp.h"

#include "inet/networklayer/common/L3AddressResolver.h"

#include <algorithm>
#include <cmath>

namespace dthsbp_v2 {

namespace {
constexpr double kPi = 3.14159265358979323846;

class ScenarioTimerMsg : public omnetpp::cMessage {
public:
    size_t eventIdx = 0;
    ScenarioTimerMsg(const char* name, size_t idx) : omnetpp::cMessage(name), eventIdx(idx) {}
};

double percentile95(std::vector<double> vals) {
    if (vals.empty()) {
        return 0.0;
    }
    std::sort(vals.begin(), vals.end());
    const size_t idx = static_cast<size_t>(std::min<double>(
        static_cast<double>(vals.size() - 1),
        std::ceil(0.95 * static_cast<double>(vals.size())) - 1.0
    ));
    return vals[idx];
}

double meanOf(const std::vector<double>& vals) {
    if (vals.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (double v : vals) {
        sum += v;
    }
    return sum / static_cast<double>(vals.size());
}
} // namespace

Define_Module(SwarmScenarioManager);

SwarmScenarioManager::~SwarmScenarioManager() {
    for (auto* m : pendingTimers_) {
        cancelAndDelete(m);
    }
    pendingTimers_.clear();
}

void SwarmScenarioManager::initialize(int stage) {
    if (stage == 0) {
        protoKind_ = parseProtoKind(par("protocol").stdstringValue());
        initialSwarmSize_ = par("initialSwarmSize");
        numClusters_ = std::max(1, static_cast<int>(par("numClusters")));
        appPort_ = par("appPort");
        mtuBytes_ = par("mtuBytes");
        qosEnabled_ = par("qosEnabled");
        ricianKDb_ = par("ricianKDb");
        simDurationSec_ = par("simDurationSec");
        joinRatePerSec_ = par("joinRatePerSec");
        leaveRatePerSec_ = par("leaveRatePerSec");
        failureRatePerSec_ = par("failureRatePerSec");
        disturbanceRatePerSec_ = par("disturbanceRatePerSec");
        slFailoverTimeSec_ = par("slFailoverTimeSec");
        precursorLeadSec_ = par("precursorLeadSec");
        dtReportIntervalSec_ = par("dtReportIntervalSec");
        chSummaryIntervalSec_ = par("chSummaryIntervalSec");
        c2BroadcastIntervalSec_ = par("c2BroadcastIntervalSec");
        missedHeartbeatTimeoutSec_ = par("missedHeartbeatTimeoutSec");
        thetaLeave_ = par("thetaLeave");
        thetaJoin_ = par("thetaJoin");
        collateralLookaheadSec_ = par("collateralLookaheadSec");
        relayJitterMaxMs_ = par("relayJitterMaxMs");
        playgroundSizeX_ = par("playgroundSizeX").doubleValue();
        playgroundSizeY_ = par("playgroundSizeY").doubleValue();
        chRingRadiusM_ = par("chRingRadiusM").doubleValue();
        clusterRadiusM_ = par("clusterRadiusM").doubleValue();

        cModule* net = getParentModule();
        totalSlots_ = net->par("numUavs");
        if (totalSlots_ < initialSwarmSize_ + 10) {
            throw omnetpp::cRuntimeError("numUavs (%d) must exceed initialSwarmSize (%d) + 10 spare slots",
                                         totalSlots_, initialSwarmSize_);
        }
        mobilities_.resize(totalSlots_, nullptr);
        apps_.resize(totalSlots_, nullptr);
        slotAddrs_.resize(totalSlots_);
        slotOccupied_.assign(totalSlots_, false);
        slotCluster_.assign(totalSlots_, -1);
        clusterHeadSlot_.assign(numClusters_, -1);
    } else if (stage == 3) {
        cModule* net = getParentModule();
        for (int i = 0; i < totalSlots_; ++i) {
            cModule* uav = net->getSubmodule("uav", i);
            mobilities_[i] = dynamic_cast<SwarmMobility*>(uav->getSubmodule("mobility"));
            apps_[i] = dynamic_cast<DTHSBPNodeApp*>(uav->getSubmodule("app", 0));
            if (!mobilities_[i] || !apps_[i]) {
                throw omnetpp::cRuntimeError("Missing mobility or app[0] on uav[%d]", i);
            }
        }

        // Schedule bootstrap at t=0.05s (after all INET NUM_INIT_STAGES finish assigning IPv4s)
        ScheduledEvent boot;
        boot.timeSec = 0.05;
        boot.action = ScheduledAction::INIT_BOOT;
        timeline_.push_back(boot);
        schedulePhysicalTimeline();

        for (size_t idx = 0; idx < timeline_.size(); ++idx) {
            auto* t = new ScenarioTimerMsg("ScenarioEvt", idx);
            pendingTimers_.push_back(t);
            scheduleAt(timeline_[idx].timeSec, t);
        }
    }
}

inet::L3Address SwarmScenarioManager::getSlotAddress(int slotIdx) const {
    if (slotIdx < 0 || slotIdx >= static_cast<int>(slotAddrs_.size())) {
        return inet::L3Address();
    }
    if (slotAddrs_[slotIdx].isUnspecified()) {
        if (cModule* net = getParentModule()) {
            if (cModule* uav = net->getSubmodule("uav", slotIdx)) {
                inet::L3AddressResolver().tryResolve(
                    uav->getFullPath().c_str(), slotAddrs_[slotIdx], inet::L3AddressResolver::ADDR_IPv4
                );
            }
        }
    }
    return slotAddrs_[slotIdx];
}

double SwarmScenarioManager::clusterCenterX(int clusterId) const {
    const double cx = playgroundSizeX_ * 0.5;
    const double ang = 2.0 * kPi * static_cast<double>(clusterId) / static_cast<double>(numClusters_);
    return cx + chRingRadiusM_ * std::cos(ang);
}

double SwarmScenarioManager::clusterCenterY(int clusterId) const {
    const double cy = playgroundSizeY_ * 0.5;
    const double ang = 2.0 * kPi * static_cast<double>(clusterId) / static_cast<double>(numClusters_);
    return cy + chRingRadiusM_ * std::sin(ang);
}

std::vector<int> SwarmScenarioManager::getInitialClusterMembers(int clusterId) const {
    std::vector<int> out;
    for (int i = 1; i < initialSwarmSize_; ++i) {
        if (slotCluster_[i] == clusterId) {
            out.push_back(i);
        }
    }
    return out;
}

int SwarmScenarioManager::getInitialClusterHead(int clusterId) const {
    if (clusterId < 0 || clusterId >= static_cast<int>(clusterHeadSlot_.size())) {
        return -1;
    }
    return clusterHeadSlot_[clusterId];
}

void SwarmScenarioManager::schedulePhysicalTimeline() {
    // Track virtual occupancy while laying out conflict-free Poisson event times over [4s, simDuration - 4s]
    std::vector<int> activeFollowers;
    for (int i = 1 + numClusters_; i < initialSwarmSize_; ++i) {
        activeFollowers.push_back(i);
    }

    const double minEvT = 3.0;
    const double maxEvT = std::max(6.0, simDurationSec_ - 2.5);

    // Sample joins
    double t = minEvT + exponential(1.0 / std::max(0.01, joinRatePerSec_));
    while (t < maxEvT) {
        ScheduledEvent ev;
        ev.timeSec = t;
        ev.action = ScheduledAction::PHYSICAL_JOIN;
        ev.clusterId = intuniform(0, numClusters_ - 1);
        timeline_.push_back(ev);
        t += exponential(1.0 / std::max(0.01, joinRatePerSec_));
    }

    // Sample planned departures (preceded by physical anomaly onset precursorLeadSec_ earlier)
    t = minEvT + 1.5 + exponential(1.0 / std::max(0.01, leaveRatePerSec_));
    size_t leaveCursor = 0;
    while (t < maxEvT && leaveCursor + 4 < activeFollowers.size()) {
        const int targetSlot = activeFollowers[leaveCursor++];
        if (std::abs(t - slFailoverTimeSec_) > 2.5) {
            ScheduledEvent pre;
            pre.timeSec = std::max(2.0, t - precursorLeadSec_);
            pre.action = ScheduledAction::PRECURSOR_LEAVE;
            pre.slotIdx = targetSlot;
            timeline_.push_back(pre);

            ScheduledEvent lev;
            lev.timeSec = t;
            lev.action = ScheduledAction::PHYSICAL_LEAVE;
            lev.slotIdx = targetSlot;
            timeline_.push_back(lev);
        }
        t += exponential(1.0 / std::max(0.01, leaveRatePerSec_));
    }

    // Sample sudden crashes (no precursor)
    t = minEvT + 2.0 + exponential(1.0 / std::max(0.01, failureRatePerSec_));
    while (t < maxEvT && leaveCursor + 2 < activeFollowers.size()) {
        const int targetSlot = activeFollowers[leaveCursor++];
        if (std::abs(t - slFailoverTimeSec_) > 2.5) {
            ScheduledEvent cr;
            cr.timeSec = t;
            cr.action = ScheduledAction::SUDDEN_CRASH;
            cr.slotIdx = targetSlot;
            timeline_.push_back(cr);
        }
        t += exponential(1.0 / std::max(0.01, failureRatePerSec_));
    }

    // Sample physical trajectory disturbances followed by forced exit 3.0s later
    t = minEvT + 1.0 + exponential(1.0 / std::max(0.01, disturbanceRatePerSec_));
    while (t < maxEvT && leaveCursor < activeFollowers.size()) {
        const int targetSlot = activeFollowers[leaveCursor++];
        if (std::abs(t - slFailoverTimeSec_) > 2.5) {
            ScheduledEvent d;
            d.timeSec = t;
            d.action = ScheduledAction::DISTURBANCE;
            d.slotIdx = targetSlot;
            timeline_.push_back(d);

            if (t + 3.0 < maxEvT) {
                ScheduledEvent lev;
                lev.timeSec = t + 3.0;
                lev.action = ScheduledAction::PHYSICAL_LEAVE;
                lev.slotIdx = targetSlot;
                timeline_.push_back(lev);
            }
        }
        t += exponential(1.0 / std::max(0.01, disturbanceRatePerSec_));
    }

    // Single Swarm Leader failover event
    if (slFailoverTimeSec_ > 5.0 && slFailoverTimeSec_ < maxEvT) {
        ScheduledEvent sl;
        sl.timeSec = slFailoverTimeSec_;
        sl.action = ScheduledAction::SL_FAILOVER;
        sl.slotIdx = 0;
        timeline_.push_back(sl);
    }

    std::stable_sort(timeline_.begin(), timeline_.end(), [](const ScheduledEvent& a, const ScheduledEvent& b) {
        return a.timeSec < b.timeSec;
    });
}

void SwarmScenarioManager::handleMessage(omnetpp::cMessage* msg) {
    auto* tmsg = dynamic_cast<ScenarioTimerMsg*>(msg);
    if (!tmsg) {
        delete msg;
        return;
    }
    const ScheduledEvent ev = timeline_[tmsg->eventIdx];
    auto it = std::find(pendingTimers_.begin(), pendingTimers_.end(), msg);
    if (it != pendingTimers_.end()) {
        pendingTimers_.erase(it);
    }
    delete msg;

    switch (ev.action) {
        case ScheduledAction::INIT_BOOT:
            executeBootstrap();
            break;
        case ScheduledAction::PRECURSOR_LEAVE:
            executePrecursor(ev.slotIdx);
            break;
        case ScheduledAction::PHYSICAL_LEAVE:
            executePhysicalLeave(ev.slotIdx);
            break;
        case ScheduledAction::SUDDEN_CRASH:
            executeSuddenCrash(ev.slotIdx);
            break;
        case ScheduledAction::DISTURBANCE:
            executeDisturbance(ev.slotIdx);
            break;
        case ScheduledAction::PHYSICAL_JOIN:
            executePhysicalJoin(ev.clusterId);
            break;
        case ScheduledAction::SL_FAILOVER:
            executeSlFailover();
            break;
    }
}

void SwarmScenarioManager::executeBootstrap() {
    cModule* net = getParentModule();
    for (int i = 0; i < totalSlots_; ++i) {
        if (slotAddrs_[i].isUnspecified()) {
            cModule* uav = net->getSubmodule("uav", i);
            inet::L3AddressResolver().tryResolve(
                uav->getFullPath().c_str(), slotAddrs_[i], inet::L3AddressResolver::ADDR_IPv4
            );
        }
        if (slotAddrs_[i].isUnspecified()) {
            throw omnetpp::cRuntimeError("Failed to resolve IPv4 address for uav[%d] at bootstrap", i);
        }
    }

    const double cx = playgroundSizeX_ * 0.5;
    const double cy = playgroundSizeY_ * 0.5;

    // Slot 0: Initial Swarm Leader
    slotOccupied_[0] = true;
    slotCluster_[0] = -1;
    mobilities_[0]->activateAt(cx, cy, cx, cy, 40.0, uniform(0.0, 2.0 * kPi), uniform(5.0, 8.0), 99.0);

    // Slots 1..numClusters_: Cluster Heads on ring around SL
    for (int c = 0; c < numClusters_; ++c) {
        const int chSlot = 1 + c;
        slotOccupied_[chSlot] = true;
        slotCluster_[chSlot] = c;
        clusterHeadSlot_[c] = chSlot;
        const double hx = clusterCenterX(c);
        const double hy = clusterCenterY(c);
        mobilities_[chSlot]->activateAt(hx, hy, hx, hy, 45.0, uniform(0.0, 2.0 * kPi), uniform(5.0, 9.0), 96.0);
    }

    // Remaining initial followers assigned round-robin to clusters
    for (int i = 1 + numClusters_; i < initialSwarmSize_; ++i) {
        const int c = (i - 1 - numClusters_) % numClusters_;
        slotOccupied_[i] = true;
        slotCluster_[i] = c;
        const double hx = clusterCenterX(c);
        const double hy = clusterCenterY(c);
        const double r = uniform(12.0, clusterRadiusM_);
        const double ang = uniform(0.0, 2.0 * kPi);
        const double x = hx + r * std::cos(ang);
        const double y = hy + r * std::sin(ang);
        const double initEnergy = uniform(55.0, 98.0);
        mobilities_[i]->activateAt(x, y, hx, hy, clusterRadiusM_, uniform(0.0, 2.0 * kPi), uniform(5.0, 11.0), initEnergy);
    }

    // Park unused slots cleanly
    for (int i = initialSwarmSize_; i < totalSlots_; ++i) {
        slotOccupied_[i] = false;
        slotCluster_[i] = -1;
        mobilities_[i]->parkOut();
    }

    activePhysicalCount_ = initialSwarmSize_;
    initializedSwarm_ = true;

    // Start protocol agents AFTER all slots know their pre-mission cluster & directory
    apps_[0]->activateNode(0, NodeRole::SWARM_LEADER, -1, 0, 0, 1);
    for (int c = 0; c < numClusters_; ++c) {
        const int chSlot = clusterHeadSlot_[c];
        apps_[chSlot]->activateNode(chSlot, NodeRole::CLUSTER_HEAD, c, chSlot, 0, 1);
    }
    for (int i = 1 + numClusters_; i < initialSwarmSize_; ++i) {
        const int c = slotCluster_[i];
        apps_[i]->activateNode(i, NodeRole::FOLLOWER, c, clusterHeadSlot_[c], 0, 1);
    }
}

void SwarmScenarioManager::executePrecursor(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= totalSlots_ || !slotOccupied_[slotIdx]) {
        return;
    }
    // Physical divergence & link quality fade prior to scheduled departure
    mobilities_[slotIdx]->injectDisturbance(18.0, 12.0, 11.0, 7.0, 8.0);
    apps_[slotIdx]->setSimulatedLinkQuality(0.40);
    auto& rec = anomalyAudit_[slotIdx];
    rec.anomalyActive = true;
    rec.excludedByProtocol = false;
    rec.startTimeSec = inet::simTime().dbl();
}

void SwarmScenarioManager::executePhysicalLeave(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= totalSlots_ || !slotOccupied_[slotIdx]) {
        return;
    }
    auto& rec = anomalyAudit_[slotIdx];
    if (!rec.anomalyActive) {
        rec.anomalyActive = true;
        rec.excludedByProtocol = false;
        rec.startTimeSec = inet::simTime().dbl();
    }
    slotOccupied_[slotIdx] = false;
    --activePhysicalCount_;
    apps_[slotIdx]->deactivateNode();
    mobilities_[slotIdx]->parkOut();
}

void SwarmScenarioManager::executeSuddenCrash(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= totalSlots_ || !slotOccupied_[slotIdx]) {
        return;
    }
    auto& rec = anomalyAudit_[slotIdx];
    rec.anomalyActive = true;
    rec.excludedByProtocol = false;
    rec.startTimeSec = inet::simTime().dbl();

    slotOccupied_[slotIdx] = false;
    --activePhysicalCount_;
    apps_[slotIdx]->deactivateNode();
    mobilities_[slotIdx]->parkOut();
}

void SwarmScenarioManager::executeDisturbance(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= totalSlots_ || !slotOccupied_[slotIdx]) {
        return;
    }
    mobilities_[slotIdx]->injectDisturbance(16.0, -14.0, 10.0, -8.0, 6.0);
    apps_[slotIdx]->setSimulatedLinkQuality(0.45);
    auto& rec = anomalyAudit_[slotIdx];
    rec.anomalyActive = true;
    rec.excludedByProtocol = false;
    rec.startTimeSec = inet::simTime().dbl();
}

void SwarmScenarioManager::executePhysicalJoin(int clusterId) {
    int freeSlot = -1;
    for (int i = 1 + numClusters_; i < totalSlots_; ++i) {
        if (!slotOccupied_[i]) {
            freeSlot = i;
            break;
        }
    }
    if (freeSlot == -1) {
        return;
    }
    const int c = std::clamp(clusterId, 0, numClusters_ - 1);
    const int chSlot = clusterHeadSlot_[c];
    const int currentSlSlot = (slotOccupied_[0]) ? 0 : clusterHeadSlot_[0];

    slotOccupied_[freeSlot] = true;
    slotCluster_[freeSlot] = c;
    ++activePhysicalCount_;

    const double hx = clusterCenterX(c);
    const double hy = clusterCenterY(c);
    const double r = uniform(15.0, clusterRadiusM_ * 0.85);
    const double ang = uniform(0.0, 2.0 * kPi);
    mobilities_[freeSlot]->activateAt(
        hx + r * std::cos(ang),
        hy + r * std::sin(ang),
        hx,
        hy,
        clusterRadiusM_,
        uniform(0.0, 2.0 * kPi),
        uniform(5.0, 10.0),
        uniform(80.0, 99.0)
    );
    // Start joiner with keyEpoch = 0 (unauthenticated / unsynced until admission AC_VO rekey completes!)
    apps_[freeSlot]->activateNode(freeSlot, NodeRole::FOLLOWER, c, chSlot, currentSlSlot, 0);
}

void SwarmScenarioManager::executeSlFailover() {
    if (!slotOccupied_[0]) {
        return;
    }
    auto& rec = anomalyAudit_[0];
    rec.anomalyActive = true;
    rec.excludedByProtocol = false;
    rec.startTimeSec = inet::simTime().dbl();

    slotOccupied_[0] = false;
    --activePhysicalCount_;
    apps_[0]->deactivateNode();
    mobilities_[0]->parkOut();
}

void SwarmScenarioManager::recordPacketTx(MsgKind kind, int bytes, bool isAck, bool isResync) {
    switch (kind) {
        case MsgKind::REKEY_BCAST_FRAG:
            rekeyBytesOnAir_ += static_cast<double>(bytes);
            ++rekeyFragmentsSent_;
            break;
        case MsgKind::REKEY_UNICAST_ACK:
            rekeyBytesOnAir_ += static_cast<double>(bytes);
            ++rekeyAcksSent_;
            break;
        case MsgKind::KEY_RESYNC_PULL:
            rekeyBytesOnAir_ += static_cast<double>(bytes);
            ++keyResyncPullsSent_;
            break;
        case MsgKind::HEARTBEAT_DT_REPORT:
        case MsgKind::CH_SUMMARY_TO_SL:
            controlDtBytesOnAir_ += static_cast<double>(bytes);
            break;
        case MsgKind::MISSION_C2_BCAST:
            c2BytesOnAir_ += static_cast<double>(bytes);
            break;
    }
    (void)isAck;
    (void)isResync;
}

void SwarmScenarioManager::recordRekeyRound(
    bool isDtProactive,
    int /*excludedNodeId*/,
    int collateralCount,
    bool isCollateralBatch,
    double cryptoLatencyMs,
    double completionLatencyMs,
    double deliveryRatio
) {
    ++rekeyRounds_;
    if (isDtProactive) {
        ++dtProactiveRounds_;
    } else {
        ++reactiveRounds_;
    }
    if (isCollateralBatch) {
        ++collateralBatches_;
        collateralLeavesBatched_ += std::max(1, collateralCount);
    }
    cryptoLatenciesMs_.push_back(cryptoLatencyMs);
    rekeyCompletionLatenciesMs_.push_back(completionLatencyMs);
    rekeyPdrSamples_.push_back(std::clamp(deliveryRatio, 0.0, 1.0));
}

void SwarmScenarioManager::recordNodeExcludedByProtocol(int uavSlotId, double simTimeSec) {
    auto it = anomalyAudit_.find(uavSlotId);
    if (it != anomalyAudit_.end() && it->second.anomalyActive && !it->second.excludedByProtocol) {
        it->second.excludedByProtocol = true;
        it->second.excludedTimeSec = simTimeSec;
    }
}

void SwarmScenarioManager::recordC2Sent(uint64_t /*requiredKeyEpoch*/, double /*simTimeSec*/) {
    ++c2BcastsSent_;
    // Count C2 exposure for any physically disturbed/departed slot not yet excluded from group keys
    for (auto& kv : anomalyAudit_) {
        if (kv.second.anomalyActive && !kv.second.excludedByProtocol) {
            ++kv.second.exposurePackets;
        }
    }
}

void SwarmScenarioManager::recordC2Delivery(bool decryptedOk, double endToEndLatencyMs) {
    ++c2ExpectedReceptions_;
    if (decryptedOk) {
        ++c2DecryptedOk_;
    }
    c2LatenciesMs_.push_back(endToEndLatencyMs);
}

void SwarmScenarioManager::recordDesyncDurationMs(double durationMs) {
    if (durationMs >= 0.0) {
        desyncDurationsMs_.push_back(durationMs);
    }
}

void SwarmScenarioManager::recordSlFailoverCompleted() {
    ++slFailoverCompleted_;
}

void SwarmScenarioManager::recordRelaySuppressed() {
    ++relaysSuppressed_;
}

void SwarmScenarioManager::finish() {
    double totalExposure = 0.0;
    long anomalyCount = 0;
    for (const auto& kv : anomalyAudit_) {
        if (kv.second.anomalyActive) {
            totalExposure += static_cast<double>(kv.second.exposurePackets);
            ++anomalyCount;
        }
    }
    const double meanExposure = (anomalyCount > 0) ? (totalExposure / static_cast<double>(anomalyCount)) : 0.0;
    const double c2Rate = (c2ExpectedReceptions_ > 0)
        ? (static_cast<double>(c2DecryptedOk_) / static_cast<double>(c2ExpectedReceptions_))
        : 1.0;
    const double dtHitRate = (rekeyRounds_ > 0)
        ? (static_cast<double>(dtProactiveRounds_) / static_cast<double>(rekeyRounds_))
        : 0.0;
    const double rekeyPdr = rekeyPdrSamples_.empty() ? 1.0 : meanOf(rekeyPdrSamples_);
    const double totalBytes = rekeyBytesOnAir_ + controlDtBytesOnAir_ + c2BytesOnAir_;

    recordScalar("protocolCode", static_cast<double>(protoKind_));
    recordScalar("initialSwarmSize", static_cast<double>(initialSwarmSize_));
    recordScalar("qosEnabled", qosEnabled_ ? 1.0 : 0.0);
    recordScalar("ricianKDb", ricianKDb_);
    recordScalar("rekeyRounds", static_cast<double>(rekeyRounds_));
    recordScalar("dtProactiveRounds", static_cast<double>(dtProactiveRounds_));
    recordScalar("collateralBatches", static_cast<double>(collateralBatches_));
    recordScalar("collateralLeavesBatched", static_cast<double>(collateralLeavesBatched_));
    recordScalar("reactiveRounds", static_cast<double>(reactiveRounds_));
    recordScalar("slFailoverCompleted", static_cast<double>(slFailoverCompleted_));
    recordScalar("dtPredictionHitRate", dtHitRate);

    recordScalar("rekeyBytesOnAir", rekeyBytesOnAir_);
    recordScalar("controlDtBytesOnAir", controlDtBytesOnAir_);
    recordScalar("c2BytesOnAir", c2BytesOnAir_);
    recordScalar("totalBytesOnAir", totalBytes);
    recordScalar("rekeyFragmentsSent", static_cast<double>(rekeyFragmentsSent_));
    recordScalar("rekeyAcksSent", static_cast<double>(rekeyAcksSent_));
    recordScalar("keyResyncPullsSent", static_cast<double>(keyResyncPullsSent_));
    recordScalar("relaysSuppressed", static_cast<double>(relaysSuppressed_));

    recordScalar("meanCryptoLatencyMs", meanOf(cryptoLatenciesMs_));
    recordScalar("meanRekeyCompletionLatencyMs", meanOf(rekeyCompletionLatenciesMs_));
    recordScalar("p95RekeyCompletionLatencyMs", percentile95(rekeyCompletionLatenciesMs_));
    recordScalar("meanC2LatencyMs", meanOf(c2LatenciesMs_));
    recordScalar("c2BcastsSent", static_cast<double>(c2BcastsSent_));
    recordScalar("c2ExpectedReceptions", static_cast<double>(c2ExpectedReceptions_));
    recordScalar("c2DecryptedOk", static_cast<double>(c2DecryptedOk_));
    recordScalar("c2DecryptionSuccessRate", c2Rate);
    recordScalar("meanExposureWindowPackets", meanExposure);
    recordScalar("totalExposureWindowPackets", totalExposure);
    recordScalar("meanDesyncOutageDurationMs", meanOf(desyncDurationsMs_));
    recordScalar("p95DesyncOutageDurationMs", percentile95(desyncDurationsMs_));
    recordScalar("rekeyPdr", rekeyPdr);
}

} // namespace dthsbp_v2
