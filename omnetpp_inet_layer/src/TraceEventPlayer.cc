#include "TraceEventPlayer.h"
#include "mini_json.hpp"

#ifndef OMNETPP_STANDALONE_TEST
#include "DTHSBPRekeyApp.h"
#include "TraceMobility.h"
#include "inet/networklayer/contract/ipv4/Ipv4Address.h"
#endif

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <queue>
#include <sstream>
#include <stdexcept>

namespace dthsbp {

namespace {

std::vector<int> parseIntArray(const json::Value& val) {
    std::vector<int> out;
    if (val.isArray()) {
        const auto& arr = val.asArray();
        out.reserve(arr.size());
        for (size_t i = 0; i < arr.size(); ++i) {
            out.push_back(arr[i].asInt());
        }
    } else if (val.isNumber()) {
        out.push_back(val.asInt());
    }
    return out;
}

std::vector<const json::Value*> asObjectElements(const json::Value& val) {
    std::vector<const json::Value*> ptrs;
    if (val.isArray()) {
        const auto& arr = val.asArray();
        ptrs.reserve(arr.size());
        for (size_t i = 0; i < arr.size(); ++i) {
            ptrs.push_back(&arr[i]);
        }
    } else if (val.isObject()) {
        ptrs.push_back(&val);
    }
    return ptrs;
}

} // namespace

void TracePlayerCore::loadFromFile(const std::string& filePath) {
    json::Value root = json::parseFile(filePath);

    const json::Value& m = root.at("meta");
    meta_ = TraceMeta();
    meta_.seed = m.at("seed").asInt();
    meta_.swarmSize = m.at("swarmSize").asInt();
    meta_.numClusters = m.at("numClusters").asInt();
    meta_.protocol = m.at("protocol").asString();
    meta_.simulationTime = m.at("simulationTime").asDouble();
    meta_.timeStep = m.at("timeStep").asDouble();
    meta_.arenaWidthM = m.at("arenaWidthM").asDouble();
    meta_.arenaHeightM = m.at("arenaHeightM").asDouble();
    meta_.maxVelocityMps = m.at("maxVelocityMps").asDouble();
    meta_.keySize = m.at("keySize").asInt();
    meta_.headerSize = m.at("headerSize").asInt();
    meta_.controlSize = m.at("controlSize").asInt();
    meta_.messageSize = m.at("messageSize").asInt();
    meta_.packetSize = m.at("packetSize").asInt();
    meta_.mtu = m.at("mtu").asInt();

    initialNodes_.clear();
    const auto initPtrs = asObjectElements(root.at("initialNodes"));
    initialNodes_.reserve(initPtrs.size());
    for (size_t i = 0; i < initPtrs.size(); ++i) {
        const json::Value& n = *initPtrs[i];
        NodeState st;
        st.uavId = n.at("uavId").asInt();
        st.clusterId = n.at("clusterId").asInt();
        st.role = n.at("role").asString();
        st.x = n.at("x").asDouble();
        st.y = n.at("y").asDouble();
        st.vx = n.at("vx").asDouble();
        st.vy = n.at("vy").asDouble();
        st.energy = n.at("energy").asDouble();
        st.active = true;
        initialNodes_.push_back(st);
    }

    events_.clear();
    const auto evPtrs = asObjectElements(root.at("events"));
    events_.reserve(evPtrs.size());
    for (size_t i = 0; i < evPtrs.size(); ++i) {
        const json::Value& e = *evPtrs[i];
        TraceEventRecord rec;
        rec.t = e.at("t").asDouble();
        rec.type = e.at("type").asString();
        rec.clusterId = e.at("clusterId").asInt();
        rec.affectedUAVs = parseIntArray(e.at("affectedUAVs"));
        rec.isCollateralBatch = e.at("isCollateralBatch").asBool();

        if (e.hasKey("predictionHit") && !e.at("predictionHit").isNull()) {
            rec.hasPredictionHit = true;
            rec.predictionHit = e.at("predictionHit").asBool();
        } else {
            rec.hasPredictionHit = false;
            rec.predictionHit = false;
        }

        const auto posPtrs = asObjectElements(e.at("nodePositions"));
        rec.nodePositions.reserve(posPtrs.size());
        for (size_t j = 0; j < posPtrs.size(); ++j) {
            const json::Value& np = *posPtrs[j];
            NodePositionSnapshot snap;
            snap.uavId = np.at("uavId").asInt();
            snap.clusterId = np.at("clusterId").asInt();
            snap.role = np.at("role").asString();
            snap.x = np.at("x").asDouble();
            snap.y = np.at("y").asDouble();
            rec.nodePositions.push_back(snap);
        }

        const auto hopPtrs = asObjectElements(e.at("hops"));
        rec.hops.reserve(hopPtrs.size());
        for (size_t j = 0; j < hopPtrs.size(); ++j) {
            const json::Value& h = *hopPtrs[j];
            HopDescriptor hd;
            hd.fromRole = h.at("from").asString();
            hd.toRole = h.at("to").asString();
            hd.bytes = h.at("bytes").asInt();
            hd.kind = h.at("kind").asString();
            hd.fanout = h.at("fanout").asInt();
            hd.fragmentsCount = h.at("fragmentsCount").asInt();
            rec.hops.push_back(hd);
        }

        const json::Value& c = e.at("crypto");
        rec.crypto.leaderTimeMs = c.at("leaderTimeMs").asDouble();
        rec.crypto.rekeyCount = c.at("rekeyCount").asInt();
        rec.crypto.messagesSent = c.at("messagesSent").asInt();
        rec.crypto.bytesTransmitted = c.at("bytesTransmitted").asInt64();

        rec.clusterMembershipAfter = parseIntArray(e.at("clusterMembershipAfter"));
        events_.push_back(rec);
    }

    resetState();
}

void TracePlayerCore::resetState() {
    nodes_.clear();
    clusterMembers_.clear();
    clusterHeads_.clear();
    slId_ = -1;

    for (int c = 1; c <= meta_.numClusters; ++c) {
        clusterMembers_[c] = std::vector<int>();
        clusterHeads_[c] = -1;
    }

    for (size_t i = 0; i < initialNodes_.size(); ++i) {
        const NodeState& init = initialNodes_[i];
        nodes_[init.uavId] = init;
        clusterMembers_[init.clusterId].push_back(init.uavId);
        if (init.role == "SL") {
            slId_ = init.uavId;
            clusterHeads_[init.clusterId] = init.uavId;
        } else if (init.role == "CH") {
            clusterHeads_[init.clusterId] = init.uavId;
        }
    }

    summary_ = PlayerSummary();
    summary_.protocol = meta_.protocol;
    summary_.swarmSize = meta_.swarmSize;
    summary_.numEvents = events_.size();
    summary_.activeUavsEnd = static_cast<size_t>(countActiveNodes());
}

void TracePlayerCore::applyEvent(size_t eventIdx) {
    if (eventIdx >= events_.size()) {
        throw std::out_of_range("TraceEventPlayer event index out of range");
    }
    const TraceEventRecord& ev = events_[eventIdx];
    const int cid = ev.clusterId;
    if (clusterMembers_.find(cid) == clusterMembers_.end()) {
        clusterMembers_[cid] = std::vector<int>();
    }

    // 1. Apply departure / arrival lifecycle transitions
    if (ev.type == "leave" || ev.type == "failure" || ev.type == "dtPredictedDeparture") {
        for (size_t i = 0; i < ev.affectedUAVs.size(); ++i) {
            auto it = nodes_.find(ev.affectedUAVs[i]);
            if (it != nodes_.end()) {
                it->second.active = false;
            }
        }
    } else if (ev.type == "join") {
        for (size_t i = 0; i < ev.affectedUAVs.size(); ++i) {
            int uid = ev.affectedUAVs[i];
            auto it = nodes_.find(uid);
            if (it == nodes_.end()) {
                NodeState st;
                st.uavId = uid;
                st.clusterId = cid;
                st.role = "follower";
                st.active = true;
                nodes_[uid] = st;
            } else {
                it->second.active = true;
                it->second.clusterId = cid;
            }
        }
    }

    // 2. Bounded nodePositions carry-forward
    std::unordered_set<int> updatedIds;
    std::unordered_map<int, std::string> snapshotRolesInCid;
    summary_.maxBoundedSnapshotSize = std::max(
        summary_.maxBoundedSnapshotSize,
        ev.nodePositions.size()
    );

    for (size_t i = 0; i < ev.nodePositions.size(); ++i) {
        const NodePositionSnapshot& snap = ev.nodePositions[i];
        updatedIds.insert(snap.uavId);
        auto it = nodes_.find(snap.uavId);
        if (it == nodes_.end()) {
            NodeState st;
            st.uavId = snap.uavId;
            st.clusterId = snap.clusterId;
            st.role = snap.role;
            st.x = snap.x;
            st.y = snap.y;
            st.active = true;
            nodes_[snap.uavId] = st;
        } else {
            it->second.x = snap.x;
            it->second.y = snap.y;
            it->second.role = snap.role;
            it->second.clusterId = snap.clusterId;
            it->second.active = true;
        }

        if (snap.role == "SL") {
            slId_ = snap.uavId;
            clusterHeads_[snap.clusterId] = snap.uavId;
        } else if (snap.role == "CH") {
            clusterHeads_[snap.clusterId] = snap.uavId;
        }
        if (snap.clusterId == cid) {
            snapshotRolesInCid[snap.uavId] = snap.role;
        }
    }

    size_t carriedThisStep = 0;
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (it->second.active && updatedIds.find(it->first) == updatedIds.end()) {
            ++carriedThisStep;
        }
    }
    summary_.carryForwardNodesPreserved += carriedThisStep;

    // 3. Reconcile exact clusterMembershipAfter
    std::unordered_set<int> newSet(
        ev.clusterMembershipAfter.begin(),
        ev.clusterMembershipAfter.end()
    );
    const std::vector<int>& prevMembers = clusterMembers_[cid];
    for (size_t i = 0; i < prevMembers.size(); ++i) {
        int uid = prevMembers[i];
        if (newSet.find(uid) == newSet.end()) {
            auto it = nodes_.find(uid);
            if (it != nodes_.end()) {
                it->second.active = false;
            }
        }
    }
    clusterMembers_[cid] = ev.clusterMembershipAfter;

    // 4. Reconcile CH / SL roles on head departure
    if (ev.clusterMembershipAfter.empty()) {
        clusterHeads_[cid] = -1;
    } else {
        int taggedHead = -1;
        for (auto it = snapshotRolesInCid.begin(); it != snapshotRolesInCid.end(); ++it) {
            if (newSet.find(it->first) != newSet.end() &&
                (it->second == "CH" || it->second == "SL")) {
                taggedHead = it->first;
                break;
            }
        }
        if (taggedHead != -1) {
            clusterHeads_[cid] = taggedHead;
        } else {
            int curCh = (clusterHeads_.find(cid) != clusterHeads_.end()) ? clusterHeads_[cid] : -1;
            if (curCh == -1 || newSet.find(curCh) == newSet.end()) {
                int newCh = ev.clusterMembershipAfter.front();
                clusterHeads_[cid] = newCh;
                bool promoteSl = (cid == 1 && (slId_ == -1 || !nodes_[slId_].active));
                nodes_[newCh].role = promoteSl ? "SL" : "CH";
                ++summary_.chReassignments;
            }
        }
    }

    if (slId_ == -1 || !nodes_[slId_].active) {
        auto c1It = clusterHeads_.find(1);
        if (c1It != clusterHeads_.end() && c1It->second != -1 && nodes_[c1It->second].active) {
            slId_ = c1It->second;
            nodes_[slId_].role = "SL";
        } else {
            slId_ = -1;
            for (auto it = clusterHeads_.begin(); it != clusterHeads_.end(); ++it) {
                if (it->second != -1 && nodes_[it->second].active) {
                    slId_ = it->second;
                    nodes_[slId_].role = "SL";
                    break;
                }
            }
        }
    }

    // 5. Accumulate trace metrics
    if (ev.crypto.rekeyCount > 0 || ev.crypto.bytesTransmitted > 0) {
        ++summary_.rekeyEvents;
    }
    if (ev.type == "dtPredictedDeparture") {
        ++summary_.dtPredictedEvents;
    }
    if (ev.isCollateralBatch) {
        ++summary_.collateralBatches;
        if (ev.affectedUAVs.size() > 1) {
            summary_.collateralLeavesBatched += (ev.affectedUAVs.size() - 1);
        }
    }
    summary_.totalCryptoBytes += ev.crypto.bytesTransmitted;
    summary_.totalMessagesSent += ev.crypto.messagesSent;
    summary_.totalLeaderTimeMs += ev.crypto.leaderTimeMs;
    for (size_t j = 0; j < ev.hops.size(); ++j) {
        summary_.totalHopBytes += ev.hops[j].bytes;
        summary_.totalFragments += ev.hops[j].fragmentsCount;
    }
    summary_.activeUavsEnd = static_cast<size_t>(countActiveNodes());
}

void TracePlayerCore::replayAll() {
    resetState();
    for (size_t i = 0; i < events_.size(); ++i) {
        applyEvent(i);
    }
}

double TracePlayerCore::distanceBetween(int uavA, int uavB) const {
    const NodeState& na = nodes_.at(uavA);
    const NodeState& nb = nodes_.at(uavB);
    double dx = na.x - nb.x;
    double dy = na.y - nb.y;
    return std::sqrt(dx * dx + dy * dy);
}

int TracePlayerCore::countActiveNodes() const {
    int count = 0;
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (it->second.active) ++count;
    }
    return count;
}

bool TracePlayerCore::hasValidHeadForAllNonEmptyClusters() const {
    for (auto it = clusterMembers_.begin(); it != clusterMembers_.end(); ++it) {
        if (it->second.empty()) continue;
        auto hIt = clusterHeads_.find(it->first);
        if (hIt == clusterHeads_.end() || hIt->second == -1) return false;
        auto nIt = nodes_.find(hIt->second);
        if (nIt == nodes_.end() || !nIt->second.active) return false;
    }
    return slId_ != -1 && nodes_.at(slId_).active;
}

std::vector<int> TracePlayerCore::activeUavIds() const {
    std::vector<int> ids;
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (it->second.active) ids.push_back(it->first);
    }
    return ids;
}

int TracePlayerCore::clusterHead(int clusterId) const {
    auto it = clusterHeads_.find(clusterId);
    if (it == clusterHeads_.end()) return -1;
    int ch = it->second;
    if (ch == -1) return -1;
    auto nit = nodes_.find(ch);
    if (nit == nodes_.end() || !nit->second.active) return -1;
    return ch;
}

std::vector<int> TracePlayerCore::clusterFollowers(int clusterId) const {
    std::vector<int> followers;
    int ch = clusterHead(clusterId);
    auto it = clusterMembers_.find(clusterId);
    if (it == clusterMembers_.end()) return followers;
    for (int uid : it->second) {
        if (uid != ch) {
            auto nit = nodes_.find(uid);
            if (nit != nodes_.end() && nit->second.active) {
                followers.push_back(uid);
            }
        }
    }
    return followers;
}

std::unordered_map<int, std::vector<int> > TracePlayerCore::computeBfsPaths(
    int srcUavId,
    double commRangeM,
    const std::vector<int>& targets
) const {
    std::unordered_map<int, std::vector<int> > out;
    auto srcIt = nodes_.find(srcUavId);
    if (srcIt == nodes_.end() || !srcIt->second.active) {
        return out;
    }

    std::unordered_set<int> targetSet(targets.begin(), targets.end());
    if (targetSet.find(srcUavId) != targetSet.end()) {
        out[srcUavId] = std::vector<int>{srcUavId};
        targetSet.erase(srcUavId);
    }
    if (targetSet.empty()) {
        return out;
    }

    const std::vector<int> act = activeUavIds();
    const size_t n = act.size();
    std::unordered_map<int, size_t> idToIdx;
    idToIdx.reserve(n * 2 + 1);
    for (size_t i = 0; i < n; ++i) {
        idToIdx[act[i]] = i;
    }

    const double r2 = commRangeM * commRangeM;
    std::vector<double> xs(n), ys(n);
    for (size_t i = 0; i < n; ++i) {
        const NodeState& st = nodes_.at(act[i]);
        xs[i] = st.x;
        ys[i] = st.y;
    }

    std::vector<int> parent(n, -1);
    std::vector<bool> visited(n, false);
    const size_t sIdx = idToIdx[srcUavId];
    visited[sIdx] = true;

    std::queue<size_t> q;
    q.push(sIdx);
    size_t remainingTargets = targetSet.size();

    while (!q.empty() && remainingTargets > 0) {
        const size_t u = q.front();
        q.pop();
        const double ux = xs[u];
        const double uy = ys[u];

        for (size_t v = 0; v < n; ++v) {
            if (visited[v]) continue;
            const double dx = ux - xs[v];
            const double dy = uy - ys[v];
            if (dx * dx + dy * dy <= r2) {
                visited[v] = true;
                parent[v] = static_cast<int>(u);
                q.push(v);

                const int vid = act[v];
                if (targetSet.find(vid) != targetSet.end()) {
                    std::vector<int> path;
                    for (int cur = static_cast<int>(v); cur != -1; cur = parent[cur]) {
                        path.push_back(act[cur]);
                    }
                    std::reverse(path.begin(), path.end());
                    out[vid] = std::move(path);
                    --remainingTargets;
                    if (remainingTargets == 0) break;
                }
            }
        }
    }

    return out;
}

#ifndef OMNETPP_STANDALONE_TEST

Define_Module(dthsbp::TraceEventPlayer);

TraceEventPlayer::~TraceEventPlayer() {
    cancelAndDelete(stepTimer_);
    cancelAndDelete(netStartTimer_);
    cancelAndDelete(stageTimeoutTimer_);
    stepTimer_ = nullptr;
    netStartTimer_ = nullptr;
    stageTimeoutTimer_ = nullptr;
}

void TraceEventPlayer::initialize(int stage) {
    if (stage == inet::INITSTAGE_LOCAL) {
        traceFile_ = par("traceFile").stdstringValue();
        eventLogFile_ = par("eventLogFile").stdstringValue();
        enableMultiHop_ = par("enableMultiHopForwarding");
        mtuBytes_ = par("mtuBytes");
        maxRetries_ = par("maxRetries");
        commRangeM_ = par("commRangeM");
        carrierSenseRangeM_ = par("carrierSenseRangeM");
        bitrateMbps_ = par("bitrateMbps");
        pLossExtra_ = par("pLossExtra");
        retryTimeoutMultiplier_ = par("retryTimeoutMultiplier");
        lossRng_.seed(static_cast<uint32_t>(par("rngSeed").intValue()));

        core_.loadFromFile(traceFile_);

        stepTimer_ = new omnetpp::cMessage("StepTimer");
        netStartTimer_ = new omnetpp::cMessage("NetStartTimer");
        stageTimeoutTimer_ = new omnetpp::cMessage("StageTimeoutTimer");

        rekeyLatencySig_ = registerSignal("rekeyLatency");
        eventPdrSig_ = registerSignal("eventPdr");
        eventKeySyncSig_ = registerSignal("eventKeySync");
    } else if (stage == inet::INITSTAGE_APPLICATION_LAYER) {
        initHostMappings();
        if (!core_.events().empty()) {
            curEvIdx_ = 0;
            scheduleAt(omnetpp::simTime() + core_.events()[0].t, stepTimer_);
        }
    }
}

void TraceEventPlayer::initHostMappings() {
    cModule* net = getParentModule();
    numUavs_ = net->par("numUavs");
    mobilities_.assign(numUavs_, nullptr);
    apps_.assign(numUavs_, nullptr);
    uavToSlot_.clear();
    slotToUav_.assign(numUavs_, -1);
    freeSlots_.clear();

    for (int i = 0; i < numUavs_; ++i) {
        cModule* host = net->getSubmodule("uav", i);
        if (!host) {
            throw omnetpp::cRuntimeError("TraceEventPlayer: missing uav[%d] submodule", i);
        }
        mobilities_[i] = dynamic_cast<TraceMobility*>(host->getSubmodule("mobility"));
        apps_[i] = dynamic_cast<DTHSBPRekeyApp*>(host->getSubmodule("app", 0));
        if (!mobilities_[i] || !apps_[i]) {
            throw omnetpp::cRuntimeError("TraceEventPlayer: uav[%d] missing TraceMobility or DTHSBPRekeyApp", i);
        }
        apps_[i]->setAssignedUavId(-1, false);
        mobilities_[i]->setTracePosition(-5000.0, -5000.0, 0.0);
        freeSlots_.push_back(i);
    }

    for (auto it = core_.nodes().begin(); it != core_.nodes().end(); ++it) {
        if (freeSlots_.empty()) {
            throw omnetpp::cRuntimeError("numUavs=%d too small for initialNodes", numUavs_);
        }
        int slot = freeSlots_.front();
        freeSlots_.pop_front();
        uavToSlot_[it->first] = slot;
        slotToUav_[slot] = it->first;
        apps_[slot]->setAssignedUavId(it->first, true);
        mobilities_[slot]->setTracePosition(it->second.x, it->second.y, 0.0);
    }
}

void TraceEventPlayer::applyHostSlotUpdatesForEvent(const TraceEventRecord& ev) {
    if (ev.type == "leave" || ev.type == "failure" || ev.type == "dtPredictedDeparture") {
        for (int uid : ev.affectedUAVs) {
            auto it = uavToSlot_.find(uid);
            if (it != uavToSlot_.end()) {
                int slot = it->second;
                apps_[slot]->setAssignedUavId(-1, false);
                mobilities_[slot]->setTracePosition(-5000.0, -5000.0, 0.0);
                slotToUav_[slot] = -1;
                freeSlots_.push_back(slot);
                uavToSlot_.erase(it);
            }
        }
    } else if (ev.type == "join") {
        for (int uid : ev.affectedUAVs) {
            if (uavToSlot_.find(uid) == uavToSlot_.end()) {
                if (freeSlots_.empty()) {
                    throw omnetpp::cRuntimeError("numUavs=%d exhausted on join of UAV %d at t=%.2f", numUavs_, uid, ev.t);
                }
                int slot = freeSlots_.front();
                freeSlots_.pop_front();
                uavToSlot_[uid] = slot;
                slotToUav_[slot] = uid;
                apps_[slot]->setAssignedUavId(uid, true);
            }
        }
    }

    // Reconcile anyone deactivated by clusterMembershipAfter difference
    for (int slot = 0; slot < numUavs_; ++slot) {
        int uid = slotToUav_[slot];
        if (uid != -1) {
            auto nIt = core_.nodes().find(uid);
            if (nIt == core_.nodes().end() || !nIt->second.active) {
                apps_[slot]->setAssignedUavId(-1, false);
                mobilities_[slot]->setTracePosition(-5000.0, -5000.0, 0.0);
                uavToSlot_.erase(uid);
                slotToUav_[slot] = -1;
                freeSlots_.push_back(slot);
            }
        }
    }
}

void TraceEventPlayer::moveSnapshotHosts(const TraceEventRecord& ev) {
    for (size_t i = 0; i < ev.nodePositions.size(); ++i) {
        const NodePositionSnapshot& snap = ev.nodePositions[i];
        auto it = uavToSlot_.find(snap.uavId);
        if (it != uavToSlot_.end()) {
            mobilities_[it->second]->setTracePosition(snap.x, snap.y, 0.0);
        }
    }
}

void TraceEventPlayer::handleMessage(omnetpp::cMessage* msg) {
    if (msg == stepTimer_) {
        beginEvent(curEvIdx_);
    } else if (msg == netStartTimer_) {
        beginNetworkPhase();
    } else if (msg == stageTimeoutTimer_) {
        handleStageTimeout();
    } else {
        delete msg;
    }
}

void TraceEventPlayer::beginEvent(size_t evIdx) {
    core_.applyEvent(evIdx);
    const TraceEventRecord& ev = core_.events()[evIdx];
    applyHostSlotUpdatesForEvent(ev);
    moveSnapshotHosts(ev);

    evBytesOnAir_ = 0;
    evFragsSent_ = 0;
    evRetx_ = 0;
    activeTxs_.clear();
    txIdToPos_.clear();
    flatTargetOk_.clear();
    currentHopPdrs_.clear();
    currentRoundSyncMasks_.clear();
    slToChOk_ = true;
    curHopIdx_ = 0;
    bcastSubphase_ = BcastSubphase::None;

    isFlatSbpEvent_ = !ev.hops.empty();
    for (const auto& h : ev.hops) {
        if (!(h.kind == "CH_TO_FOLLOWER" && h.fromRole == "SL")) {
            isFlatSbpEvent_ = false;
            break;
        }
    }

    const double cryptoWaitS = std::max(0.0, ev.crypto.leaderTimeMs / 1000.0);
    scheduleAt(omnetpp::simTime() + cryptoWaitS, netStartTimer_);
}

void TraceEventPlayer::beginNetworkPhase() {
    evNetStartTime_ = omnetpp::simTime();
    const TraceEventRecord& ev = core_.events()[curEvIdx_];

    if (ev.hops.empty()) {
        completeCurrentEvent();
        return;
    }

    if (isFlatSbpEvent_) {
        const int slId = core_.slId();
        if (slId == -1 || uavToSlot_.find(slId) == uavToSlot_.end()) {
            completeCurrentEvent();
            return;
        }
        std::vector<int> pool;
        for (int uid : core_.activeUavIds()) {
            if (uid != slId) pool.push_back(uid);
        }
        if (pool.empty()) {
            completeCurrentEvent();
            return;
        }

        const std::unordered_map<int, std::vector<int> > bfsPaths =
            enableMultiHop_ ? core_.computeBfsPaths(slId, commRangeM_, pool) : std::unordered_map<int, std::vector<int> >();

        const int payloadBytes = ev.hops[0].bytes;
        const size_t nTargets = ev.hops.size();
        flatTargetOk_.assign(nTargets, false);
        activeTxs_.clear();
        txIdToPos_.clear();

        int launchedBits = 0;
        for (size_t i = 0; i < nTargets; ++i) {
            const int dstId = pool[i % pool.size()];
            std::vector<int> path;
            if (!enableMultiHop_ || core_.distanceBetween(slId, dstId) <= commRangeM_) {
                path = {slId, dstId};
            } else {
                auto pIt = bfsPaths.find(dstId);
                path = (pIt != bfsPaths.end()) ? pIt->second : std::vector<int>{slId, dstId};
            }

            ActiveTx tx;
            tx.txId = nextTxId_++;
            tx.isBroadcast = false;
            tx.srcUavId = path[0];
            tx.payloadBytes = payloadBytes;
            tx.numFrags = std::max(1, (payloadBytes + mtuBytes_ - 1) / mtuBytes_);
            tx.attempt = 0;
            tx.flatPathIdx = static_cast<int>(i);
            tx.fullPathPos = 0;
            tx.fullPath = std::move(path);

            const int hop0Dst = tx.fullPath[1];
            if (core_.distanceBetween(tx.srcUavId, hop0Dst) <= commRangeM_) {
                tx.expectedInRange.push_back(hop0Dst);
                tx.fragAcked[hop0Dst] = std::vector<bool>(tx.numFrags, false);
            } else {
                // Directly unreachable in-range (no BFS path exists)
                tx.finished = true;
            }

            size_t pos = activeTxs_.size();
            activeTxs_.push_back(std::move(tx));
            txIdToPos_[activeTxs_[pos].txId] = pos;
        }

        bool anyReachable = false;
        for (size_t pos = 0; pos < activeTxs_.size(); ++pos) {
            ActiveTx& tx = activeTxs_[pos];
            std::vector<int> recvs{tx.fullPath[1]};
            dispatchTx(tx, recvs);
            launchedBits += payloadBytes * 8;
            if (!tx.finished) {
                anyReachable = true;
            }
        }

        if (!anyReachable) {
            completeCurrentEvent();
        } else {
            armStageTimer(launchedBits, static_cast<int>(activeTxs_.size()));
        }
        return;
    }

    startNextHopInEvent();
}

void TraceEventPlayer::startNextHopInEvent() {
    const TraceEventRecord& ev = core_.events()[curEvIdx_];
    while (curHopIdx_ < ev.hops.size()) {
        const HopDescriptor& hop = ev.hops[curHopIdx_];
        const int slId = core_.slId();
        const int chId = core_.clusterHead(ev.clusterId);

        if (hop.kind == "FOLLOWER_TO_CH_DT") {
            int srcId = -1;
            for (int uid : ev.affectedUAVs) {
                if (uid != chId && core_.nodes().find(uid) != core_.nodes().end() && core_.nodes().at(uid).active) {
                    srcId = uid;
                    break;
                }
            }
            if (srcId != -1 && chId != -1) {
                std::vector<int> path{srcId, chId};
                if (enableMultiHop_ && core_.distanceBetween(srcId, chId) > commRangeM_) {
                    auto bfs = core_.computeBfsPaths(srcId, commRangeM_, std::vector<int>{chId});
                    if (bfs.find(chId) != bfs.end()) path = bfs[chId];
                }
                activeTxs_.clear();
                txIdToPos_.clear();
                ActiveTx tx;
                tx.txId = nextTxId_++;
                tx.isBroadcast = false;
                tx.srcUavId = path[0];
                tx.payloadBytes = hop.bytes;
                tx.numFrags = std::max(1, (hop.bytes + mtuBytes_ - 1) / mtuBytes_);
                tx.flatPathIdx = -2; // tag as FOLLOWER_TO_CH_DT
                tx.fullPathPos = 0;
                tx.fullPath = std::move(path);
                if (core_.distanceBetween(tx.fullPath[0], tx.fullPath[1]) <= commRangeM_) {
                    tx.expectedInRange.push_back(tx.fullPath[1]);
                    tx.fragAcked[tx.fullPath[1]] = std::vector<bool>(tx.numFrags, false);
                } else {
                    tx.finished = true;
                }
                activeTxs_.push_back(tx);
                txIdToPos_[tx.txId] = 0;
                dispatchTx(activeTxs_[0], std::vector<int>{activeTxs_[0].fullPath[1]});
                if (!activeTxs_[0].finished) {
                    armStageTimer(hop.bytes * 8, 1);
                    return;
                }
                currentHopPdrs_.push_back(0.0);
            }
            ++curHopIdx_;
            continue;
        } else if (hop.kind == "SL_TO_CH") {
            if (slId != -1 && chId != -1 && slId != chId) {
                std::vector<int> path{slId, chId};
                if (enableMultiHop_ && core_.distanceBetween(slId, chId) > commRangeM_) {
                    auto bfs = core_.computeBfsPaths(slId, commRangeM_, std::vector<int>{chId});
                    if (bfs.find(chId) != bfs.end()) path = bfs[chId];
                }
                activeTxs_.clear();
                txIdToPos_.clear();
                ActiveTx tx;
                tx.txId = nextTxId_++;
                tx.isBroadcast = false;
                tx.srcUavId = path[0];
                tx.payloadBytes = hop.bytes;
                tx.numFrags = std::max(1, (hop.bytes + mtuBytes_ - 1) / mtuBytes_);
                tx.flatPathIdx = -3; // tag as SL_TO_CH
                tx.fullPathPos = 0;
                tx.fullPath = std::move(path);
                if (core_.distanceBetween(tx.fullPath[0], tx.fullPath[1]) <= commRangeM_) {
                    tx.expectedInRange.push_back(tx.fullPath[1]);
                    tx.fragAcked[tx.fullPath[1]] = std::vector<bool>(tx.numFrags, false);
                } else {
                    tx.finished = true;
                }
                activeTxs_.push_back(tx);
                txIdToPos_[tx.txId] = 0;
                dispatchTx(activeTxs_[0], std::vector<int>{activeTxs_[0].fullPath[1]});
                if (!activeTxs_[0].finished) {
                    armStageTimer(hop.bytes * 8, 1);
                    return;
                }
                slToChOk_ = false;
                currentHopPdrs_.push_back(0.0);
            } else {
                // Cluster 1 loopback: SL is CH
                slToChOk_ = true;
                currentHopPdrs_.push_back(1.0);
            }
            ++curHopIdx_;
            continue;
        } else if (hop.kind == "CH_TO_FOLLOWER") {
            bcastChId_ = chId;
            bcastBytes_ = hop.bytes;
            bcastFollowers_ = core_.clusterFollowers(ev.clusterId);
            if (bcastChId_ == -1 || bcastFollowers_.empty()) {
                currentHopPdrs_.push_back(1.0);
                ++curHopIdx_;
                continue;
            }
            if (!slToChOk_) {
                currentRoundSyncMasks_.push_back(std::vector<bool>(bcastFollowers_.size(), false));
                currentHopPdrs_.push_back(0.0);
                ++curHopIdx_;
                continue;
            }
            launchBroadcastDirectWave();
            return;
        } else {
            ++curHopIdx_;
        }
    }

    completeCurrentEvent();
}

void TraceEventPlayer::launchBroadcastDirectWave() {
    bcastSubphase_ = BcastSubphase::DirectWave;
    bcastDelivered_.assign(bcastFollowers_.size(), false);
    bcastFollowerIndex_.clear();
    for (size_t i = 0; i < bcastFollowers_.size(); ++i) {
        bcastFollowerIndex_[bcastFollowers_[i]] = i;
    }

    std::vector<int> inRange;
    std::vector<int> outRange;
    for (int fid : bcastFollowers_) {
        if (core_.distanceBetween(bcastChId_, fid) <= commRangeM_) {
            inRange.push_back(fid);
        } else {
            outRange.push_back(fid);
        }
    }

    bcastRelayPaths_.clear();
    if (!outRange.empty() && enableMultiHop_) {
        bcastRelayPaths_ = core_.computeBfsPaths(bcastChId_, commRangeM_, outRange);
    }

    std::unordered_set<int> batchSet;
    std::vector<int> directReachable;
    for (int fid : inRange) {
        if (batchSet.insert(fid).second) directReachable.push_back(fid);
    }
    bcastMaxDepth_ = 0;
    for (auto it = bcastRelayPaths_.begin(); it != bcastRelayPaths_.end(); ++it) {
        const int depth = static_cast<int>(it->second.size()) - 1;
        if (depth > bcastMaxDepth_) bcastMaxDepth_ = depth;
        if (it->second.size() > 1) {
            int relay1 = it->second[1];
            if (core_.distanceBetween(bcastChId_, relay1) <= commRangeM_ && batchSet.insert(relay1).second) {
                directReachable.push_back(relay1);
            }
        }
    }

    bcastCoveredFrontier_.clear();
    bcastCoveredFrontier_.insert(bcastChId_);

    activeTxs_.clear();
    txIdToPos_.clear();
    ActiveTx tx;
    tx.txId = nextTxId_++;
    tx.isBroadcast = true;
    tx.srcUavId = bcastChId_;
    tx.payloadBytes = bcastBytes_;
    tx.numFrags = std::max(1, (bcastBytes_ + mtuBytes_ - 1) / mtuBytes_);
    tx.expectedInRange = directReachable;
    for (int rid : directReachable) {
        tx.fragAcked[rid] = std::vector<bool>(tx.numFrags, false);
    }
    tx.finished = directReachable.empty();
    activeTxs_.push_back(tx);
    txIdToPos_[tx.txId] = 0;

    dispatchTx(activeTxs_[0], directReachable);

    if (activeTxs_[0].finished) {
        bcastCurrentDepth_ = 1;
        launchBroadcastRelayWave();
    } else {
        armStageTimer(bcastBytes_ * 8, 1);
    }
}

void TraceEventPlayer::launchBroadcastRelayWave() {
    bcastSubphase_ = BcastSubphase::RelayWave;
    while (bcastCurrentDepth_ < bcastMaxDepth_) {
        const int edgeIdx = bcastCurrentDepth_;
        std::unordered_set<int> uncovered;
        std::vector<int> candParents;
        std::unordered_set<int> parentSeen;

        for (auto it = bcastRelayPaths_.begin(); it != bcastRelayPaths_.end(); ++it) {
            const auto& p = it->second;
            if (static_cast<int>(p.size()) - 1 > edgeIdx) {
                int parId = p[edgeIdx];
                int childId = p[edgeIdx + 1];
                if (bcastCoveredFrontier_.find(parId) != bcastCoveredFrontier_.end()) {
                    if (bcastCoveredFrontier_.find(childId) == bcastCoveredFrontier_.end()) {
                        uncovered.insert(childId);
                    }
                    if (parentSeen.insert(parId).second) {
                        candParents.push_back(parId);
                    }
                }
            }
        }

        std::vector<std::pair<int, std::vector<int> > > relayOps;
        while (!uncovered.empty() && !candParents.empty()) {
            int bestPar = -1;
            std::vector<int> bestChildren;
            for (int parId : candParents) {
                std::vector<int> kids;
                for (int cId : uncovered) {
                    if (core_.distanceBetween(parId, cId) <= commRangeM_) {
                        kids.push_back(cId);
                    }
                }
                if (kids.size() > bestChildren.size()) {
                    bestPar = parId;
                    bestChildren = std::move(kids);
                }
            }
            if (bestPar == -1 || bestChildren.empty()) break;
            candParents.erase(std::remove(candParents.begin(), candParents.end(), bestPar), candParents.end());
            for (int cId : bestChildren) uncovered.erase(cId);
            relayOps.push_back(std::make_pair(bestPar, std::move(bestChildren)));
        }

        ++bcastCurrentDepth_;
        if (relayOps.empty()) continue;

        activeTxs_.clear();
        txIdToPos_.clear();
        int totalBits = 0;
        for (size_t i = 0; i < relayOps.size(); ++i) {
            ActiveTx tx;
            tx.txId = nextTxId_++;
            tx.isBroadcast = true;
            tx.srcUavId = relayOps[i].first;
            tx.payloadBytes = bcastBytes_;
            tx.numFrags = std::max(1, (bcastBytes_ + mtuBytes_ - 1) / mtuBytes_);
            tx.expectedInRange = relayOps[i].second;
            for (int cid : tx.expectedInRange) {
                tx.fragAcked[cid] = std::vector<bool>(tx.numFrags, false);
            }
            size_t pos = activeTxs_.size();
            activeTxs_.push_back(std::move(tx));
            txIdToPos_[activeTxs_[pos].txId] = pos;
            totalBits += bcastBytes_ * 8;
        }
        for (size_t pos = 0; pos < activeTxs_.size(); ++pos) {
            dispatchTx(activeTxs_[pos], activeTxs_[pos].expectedInRange);
        }
        armStageTimer(totalBits, static_cast<int>(activeTxs_.size()));
        return;
    }

    // Done with CH_TO_FOLLOWER hop
    int deliveredCount = 0;
    for (bool ok : bcastDelivered_) {
        if (ok) ++deliveredCount;
    }
    const double hopPdr = bcastFollowers_.empty()
        ? 1.0
        : static_cast<double>(deliveredCount) / static_cast<double>(bcastFollowers_.size());
    currentHopPdrs_.push_back(hopPdr);
    currentRoundSyncMasks_.push_back(bcastDelivered_);
    bcastSubphase_ = BcastSubphase::None;
    ++curHopIdx_;
    startNextHopInEvent();
}

void TraceEventPlayer::dispatchTx(ActiveTx& tx, const std::vector<int>& /*receivers*/) {
    auto sIt = uavToSlot_.find(tx.srcUavId);
    if (sIt == uavToSlot_.end()) return;
    DTHSBPRekeyApp* srcApp = apps_[sIt->second];

    inet::L3Address destAddr;
    int intendedDst = -1;
    if (tx.isBroadcast) {
        destAddr = inet::L3Address(inet::Ipv4Address::ALLONES_ADDRESS);
        intendedDst = -1;
    } else {
        intendedDst = tx.fullPath[tx.fullPathPos + 1];
        auto dIt = uavToSlot_.find(intendedDst);
        if (dIt == uavToSlot_.end()) return;
        destAddr = apps_[dIt->second]->localAddress();
        if (destAddr.isUnspecified()) return;
    }

    WireRekeyHeader hdr;
    hdr.txId = tx.txId;
    hdr.eventIdx = static_cast<int32_t>(curEvIdx_);
    hdr.srcUavId = tx.srcUavId;
    hdr.senderHostIdx = sIt->second;
    hdr.intendedDstUavId = intendedDst;
    hdr.fragIdx = 0;
    hdr.numFrags = tx.numFrags;

    srcApp->sendRekeyFragments(destAddr, 5000, hdr, tx.payloadBytes, mtuBytes_);

    const int headerPerFrag = 28 /*802.11 MAC+LLC*/ + 28 /*IPv4+UDP*/;
    evFragsSent_ += tx.numFrags;
    evBytesOnAir_ += static_cast<int64_t>(tx.payloadBytes) + static_cast<int64_t>(tx.numFrags) * headerPerFrag;
}

void TraceEventPlayer::armStageTimer(int totalPayloadBitsEstimate, int txCount) {
    if (stageTimeoutTimer_->isScheduled()) {
        cancelEvent(stageTimeoutTimer_);
    }
    const double rateBps = std::max(1.0, bitrateMbps_) * 1e6;
    const double serSec = static_cast<double>(totalPayloadBitsEstimate) / rateBps;
    const double perTxGuard = (bitrateMbps_ <= 12.0) ? 0.008 : 0.003;
    const double baseFloor = (bitrateMbps_ <= 12.0) ? 0.015 : 0.006;
    const double timeoutSec = std::min(1.5, std::max(baseFloor, serSec * retryTimeoutMultiplier_ + txCount * perTxGuard));
    scheduleAt(omnetpp::simTime() + timeoutSec, stageTimeoutTimer_);
}

bool TraceEventPlayer::dropByExtraChannelLoss(int srcUavId, int dstUavId) {
    if (pLossExtra_ <= 0.0) return false;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(srcUavId)) << 32) |
                         static_cast<uint32_t>(dstUavId);
    bool& bad = geBadState_[key];
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    if (bad) {
        if (u01(lossRng_) < 0.35) bad = false;
    } else {
        if (u01(lossRng_) < 0.08) bad = true;
    }
    const double pDrop = bad
        ? std::min(0.85, std::max(pLossExtra_, pLossExtra_ * 2.5))
        : (pLossExtra_ * 0.6);
    return u01(lossRng_) < pDrop;
}

void TraceEventPlayer::onPacketArrived(int /*hostIndex*/, int uavId, const WireRekeyHeader& hdr, int /*packetBytes*/) {
    Enter_Method_Silent();
    if (hdr.eventIdx != static_cast<int32_t>(curEvIdx_)) return;
    if (hdr.intendedDstUavId != -1 && hdr.intendedDstUavId != uavId) return;

    auto mapIt = txIdToPos_.find(hdr.txId);
    if (mapIt == txIdToPos_.end()) return;
    ActiveTx& tx = activeTxs_[mapIt->second];
    if (tx.finished) return;

    if (dropByExtraChannelLoss(hdr.srcUavId, uavId)) {
        return;
    }

    auto ackIt = tx.fragAcked.find(uavId);
    if (ackIt == tx.fragAcked.end()) {
        return; // sender outside range / unexpected node
    }
    if (hdr.fragIdx >= 0 && hdr.fragIdx < static_cast<int32_t>(ackIt->second.size())) {
        ackIt->second[hdr.fragIdx] = true;
    }

    bool allFragsOk = true;
    for (bool got : ackIt->second) {
        if (!got) {
            allFragsOk = false;
            break;
        }
    }
    if (!allFragsOk) return;
    if (!tx.completedUavs.insert(uavId).second) return;

    if (tx.isBroadcast) {
        bcastCoveredFrontier_.insert(uavId);
        auto fIt = bcastFollowerIndex_.find(uavId);
        if (fIt != bcastFollowerIndex_.end()) {
            bcastDelivered_[fIt->second] = true;
        }
        if (tx.completedUavs.size() >= tx.expectedInRange.size()) {
            tx.finished = true;
        }
    } else {
        if (tx.fullPathPos + 1 >= tx.fullPath.size() - 1) {
            // Final destination of this path succeeded
            tx.finished = true;
            if (tx.flatPathIdx >= 0) {
                flatTargetOk_[tx.flatPathIdx] = true;
            } else if (tx.flatPathIdx == -2) {
                currentHopPdrs_.push_back(1.0);
            } else if (tx.flatPathIdx == -3) {
                slToChOk_ = true;
                currentHopPdrs_.push_back(1.0);
            }
        } else {
            // Spawn next hop relay immediately via real UDP/802.11
            ++tx.fullPathPos;
            tx.srcUavId = tx.fullPath[tx.fullPathPos];
            const int nextDst = tx.fullPath[tx.fullPathPos + 1];
            tx.attempt = 0;
            tx.expectedInRange.clear();
            tx.fragAcked.clear();
            tx.completedUavs.clear();
            if (core_.distanceBetween(tx.srcUavId, nextDst) <= commRangeM_) {
                tx.expectedInRange.push_back(nextDst);
                tx.fragAcked[nextDst] = std::vector<bool>(tx.numFrags, false);
                txIdToPos_.erase(tx.txId);
                tx.txId = nextTxId_++;
                txIdToPos_[tx.txId] = mapIt->second;
                dispatchTx(tx, std::vector<int>{nextDst});
            } else {
                tx.finished = true;
                if (tx.flatPathIdx == -2) currentHopPdrs_.push_back(0.0);
                else if (tx.flatPathIdx == -3) {
                    slToChOk_ = false;
                    currentHopPdrs_.push_back(0.0);
                }
            }
        }
    }

    checkCurrentStageCompletion();
}

void TraceEventPlayer::checkCurrentStageCompletion() {
    for (const auto& tx : activeTxs_) {
        if (!tx.finished) return;
    }
    if (stageTimeoutTimer_->isScheduled()) {
        cancelEvent(stageTimeoutTimer_);
    }

    if (isFlatSbpEvent_) {
        completeCurrentEvent();
    } else if (bcastSubphase_ == BcastSubphase::DirectWave) {
        bcastCurrentDepth_ = 1;
        launchBroadcastRelayWave();
    } else if (bcastSubphase_ == BcastSubphase::RelayWave) {
        launchBroadcastRelayWave();
    } else {
        ++curHopIdx_;
        startNextHopInEvent();
    }
}

void TraceEventPlayer::handleStageTimeout() {
    bool anyRetrying = false;
    int retryBits = 0;
    int retryCount = 0;

    for (size_t pos = 0; pos < activeTxs_.size(); ++pos) {
        ActiveTx& tx = activeTxs_[pos];
        if (tx.finished) continue;

        if (tx.attempt < maxRetries_) {
            ++tx.attempt;
            ++evRetx_;
            txIdToPos_.erase(tx.txId);
            tx.txId = nextTxId_++;
            txIdToPos_[tx.txId] = pos;
            dispatchTx(tx, tx.expectedInRange);
            anyRetrying = true;
            retryBits += tx.payloadBytes * 8;
            ++retryCount;
        } else {
            tx.finished = true;
            if (!tx.isBroadcast) {
                if (tx.flatPathIdx == -2) currentHopPdrs_.push_back(0.0);
                else if (tx.flatPathIdx == -3) {
                    slToChOk_ = false;
                    currentHopPdrs_.push_back(0.0);
                }
            }
        }
    }

    if (anyRetrying) {
        armStageTimer(retryBits, retryCount);
    } else {
        checkCurrentStageCompletion();
    }
}

void TraceEventPlayer::completeCurrentEvent() {
    const TraceEventRecord& ev = core_.events()[curEvIdx_];
    const double netMs = std::max(0.0, (omnetpp::simTime() - evNetStartTime_).dbl() * 1000.0);
    const double cryptoMs = ev.crypto.leaderTimeMs;
    const double totMs = cryptoMs + netMs;

    double meanPdr = 1.0;
    double keySync = 1.0;
    if (isFlatSbpEvent_) {
        int okCnt = 0;
        for (bool ok : flatTargetOk_) {
            if (ok) ++okCnt;
        }
        if (!flatTargetOk_.empty()) {
            meanPdr = static_cast<double>(okCnt) / static_cast<double>(flatTargetOk_.size());
            keySync = meanPdr;
        }
    } else {
        if (!currentHopPdrs_.empty()) {
            double s = 0.0;
            for (double v : currentHopPdrs_) s += v;
            meanPdr = s / static_cast<double>(currentHopPdrs_.size());
        }
        if (!currentRoundSyncMasks_.empty()) {
            const size_t n = currentRoundSyncMasks_[0].size();
            int synced = 0;
            for (size_t i = 0; i < n; ++i) {
                bool ok = true;
                for (const auto& mask : currentRoundSyncMasks_) {
                    if (i >= mask.size() || !mask[i]) {
                        ok = false;
                        break;
                    }
                }
                if (ok) ++synced;
            }
            keySync = (n > 0) ? (static_cast<double>(synced) / static_cast<double>(n)) : 1.0;
        } else if (ev.crypto.rekeyCount > 0) {
            keySync = slToChOk_ ? 1.0 : 0.0;
        }
    }

    EventPerfRecord rec;
    rec.t = ev.t;
    rec.eventType = ev.type;
    rec.clusterId = ev.clusterId;
    rec.rekeyCount = ev.crypto.rekeyCount;
    rec.isCollateralBatch = ev.isCollateralBatch;
    rec.affectedCount = static_cast<int>(ev.affectedUAVs.size());
    rec.hasPredictionHit = ev.hasPredictionHit ? 1 : 0;
    rec.predictionHit = (ev.hasPredictionHit && ev.predictionHit) ? 1 : 0;
    rec.cryptoBytes = ev.crypto.bytesTransmitted;
    rec.bytesOnAir = evBytesOnAir_;
    rec.fragmentsSent = evFragsSent_;
    rec.retransmissions = evRetx_;
    rec.cryptoLatencyMs = cryptoMs;
    rec.networkLatencyMs = netMs;
    rec.totalLatencyMs = totMs;
    rec.pdr = meanPdr;
    rec.keySyncRatio = keySync;
    perfRecords_.push_back(rec);

    if (ev.crypto.rekeyCount > 0) {
        emit(rekeyLatencySig_, totMs);
        emit(eventPdrSig_, meanPdr);
        emit(eventKeySyncSig_, keySync);
    }

    ++curEvIdx_;
    if (curEvIdx_ < core_.events().size()) {
        omnetpp::simtime_t nextT(core_.events()[curEvIdx_].t);
        if (nextT <= omnetpp::simTime()) {
            nextT = omnetpp::simTime();
        }
        scheduleAt(nextT, stepTimer_);
    }
}

void TraceEventPlayer::finish() {
    std::vector<double> rekeyTot;
    double sumCryptoMs = 0.0;
    double sumNetMs = 0.0;
    double sumTotMs = 0.0;
    double sumPdr = 0.0;
    double sumSync = 0.0;

    int64_t rekeyEvents = 0;
    int64_t dtPredEvents = 0;
    int64_t dtHasPred = 0;
    int64_t dtHits = 0;
    int64_t colBatches = 0;
    int64_t colLeavesBatched = 0;
    int64_t totalCryptoBytes = 0;
    int64_t totalBytesAir = 0;
    int64_t totalFrags = 0;
    int64_t totalRetx = 0;

    for (const auto& r : perfRecords_) {
        totalCryptoBytes += r.cryptoBytes;
        totalBytesAir += r.bytesOnAir;
        totalFrags += r.fragmentsSent;
        totalRetx += r.retransmissions;
        if (r.eventType == "dtPredictedDeparture") {
            ++dtPredEvents;
        }
        if (r.rekeyCount > 0) {
            ++rekeyEvents;
            sumCryptoMs += r.cryptoLatencyMs;
            sumNetMs += r.networkLatencyMs;
            sumTotMs += r.totalLatencyMs;
            sumPdr += r.pdr;
            sumSync += r.keySyncRatio;
            rekeyTot.push_back(r.totalLatencyMs);
            if (r.hasPredictionHit) {
                ++dtHasPred;
                if (r.predictionHit) ++dtHits;
            }
            if (r.isCollateralBatch) {
                ++colBatches;
                if (r.affectedCount > 1) colLeavesBatched += (r.affectedCount - 1);
            }
        }
    }

    double meanCryptoMs = (rekeyEvents > 0) ? (sumCryptoMs / rekeyEvents) : 0.0;
    double meanNetMs = (rekeyEvents > 0) ? (sumNetMs / rekeyEvents) : 0.0;
    double meanTotMs = (rekeyEvents > 0) ? (sumTotMs / rekeyEvents) : 0.0;
    double meanPdr = (rekeyEvents > 0) ? (sumPdr / rekeyEvents) : 1.0;
    double meanSync = (rekeyEvents > 0) ? (sumSync / rekeyEvents) : 1.0;
    double dtHitRate = (dtHasPred > 0) ? (static_cast<double>(dtHits) / dtHasPred) : 0.0;

    double p95TotMs = 0.0;
    if (!rekeyTot.empty()) {
        std::sort(rekeyTot.begin(), rekeyTot.end());
        const double idxReal = 0.95 * static_cast<double>(rekeyTot.size() - 1);
        const size_t lo = static_cast<size_t>(std::floor(idxReal));
        const size_t hi = std::min(rekeyTot.size() - 1, lo + 1);
        const double frac = idxReal - static_cast<double>(lo);
        p95TotMs = rekeyTot[lo] * (1.0 - frac) + rekeyTot[hi] * frac;
    }

    recordScalar("swarmSize", core_.meta().swarmSize);
    recordScalar("pLossExtra", pLossExtra_);
    recordScalar("dataRateMbps", bitrateMbps_);
    recordScalar("commRangeM", commRangeM_);
    recordScalar("numEvents", static_cast<double>(perfRecords_.size()));
    recordScalar("rekeyEvents", static_cast<double>(rekeyEvents));
    recordScalar("dtPredictedEvents", static_cast<double>(dtPredEvents));
    recordScalar("collateralBatches", static_cast<double>(colBatches));
    recordScalar("collateralLeavesBatched", static_cast<double>(colLeavesBatched));
    recordScalar("dtPredictionHitRate", dtHitRate);
    recordScalar("totalCryptoBytesMatlab", static_cast<double>(totalCryptoBytes));
    recordScalar("totalBytesOnAir", static_cast<double>(totalBytesAir));
    recordScalar("fragmentsSent", static_cast<double>(totalFrags));
    recordScalar("retransmissions", static_cast<double>(totalRetx));
    recordScalar("meanCryptoLatencyMs", meanCryptoMs);
    recordScalar("meanNetworkLatencyMs", meanNetMs);
    recordScalar("meanRekeyCompletionLatencyMs", meanTotMs);
    recordScalar("p95RekeyCompletionLatencyMs", p95TotMs);
    recordScalar("meanPdr", meanPdr);
    recordScalar("meanKeySyncRatio", meanSync);

    if (!eventLogFile_.empty()) {
        std::ofstream ofs(eventLogFile_.c_str(), std::ios::out | std::ios::trunc);
        if (ofs.is_open()) {
            ofs << "protocol,swarm_size,p_loss,data_rate_mbps,t,event_type,cluster_id,"
                   "rekey_count,is_collateral_batch,affected_count,prediction_hit,"
                   "crypto_bytes,bytes_on_air,fragments_sent,retransmissions,"
                   "crypto_latency_ms,network_latency_ms,rekey_completion_latency_ms,"
                   "pdr,key_sync_ratio\n";
            for (const auto& r : perfRecords_) {
                ofs << core_.meta().protocol << ","
                    << core_.meta().swarmSize << ","
                    << pLossExtra_ << ","
                    << bitrateMbps_ << ","
                    << r.t << ","
                    << r.eventType << ","
                    << r.clusterId << ","
                    << r.rekeyCount << ","
                    << (r.isCollateralBatch ? "true" : "false") << ","
                    << r.affectedCount << ",";
                if (r.hasPredictionHit) ofs << (r.predictionHit ? "true" : "false");
                ofs << ","
                    << r.cryptoBytes << ","
                    << r.bytesOnAir << ","
                    << r.fragmentsSent << ","
                    << r.retransmissions << ","
                    << std::fixed << std::setprecision(6)
                    << r.cryptoLatencyMs << ","
                    << r.networkLatencyMs << ","
                    << r.totalLatencyMs << ","
                    << r.pdr << ","
                    << r.keySyncRatio << "\n";
            }
        }
    }
}

#endif // OMNETPP_STANDALONE_TEST

} // namespace dthsbp
