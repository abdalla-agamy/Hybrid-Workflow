#include "TraceEventPlayer.h"
#include "mini_json.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

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

    // 2. Bounded nodePositions carry-forward: update ONLY scoped snapshot nodes,
    //    preserving untouched clusters' coordinates across time steps.
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

    // Count active carry-forward UAVs whose coordinates remain preserved from earlier events
    size_t carriedThisStep = 0;
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (it->second.active && updatedIds.find(it->first) == updatedIds.end()) {
            ++carriedThisStep;
        }
    }
    summary_.carryForwardNodesPreserved += carriedThisStep;

    // 3. Reconcile exact clusterMembershipAfter for target cluster
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

#ifndef OMNETPP_STANDALONE_TEST

Define_Module(dthsbp::TraceEventPlayer);

TraceEventPlayer::~TraceEventPlayer() {
    cancelAndDelete(stepTimer_);
    stepTimer_ = nullptr;
}

void TraceEventPlayer::initialize() {
    const std::string traceFile = par("traceFile").stdstringValue();
    core_.loadFromFile(traceFile);
    stepTimer_ = new omnetpp::cMessage("TraceEventStep");
    nextEventIdx_ = 0;
    scheduleNextTraceEvent();
}

void TraceEventPlayer::scheduleNextTraceEvent() {
    if (nextEventIdx_ >= core_.events().size()) {
        return;
    }
    omnetpp::simtime_t targetTime(core_.events()[nextEventIdx_].t);
    if (targetTime < omnetpp::simTime()) {
        targetTime = omnetpp::simTime();
    }
    scheduleAt(targetTime, stepTimer_);
}

void TraceEventPlayer::handleMessage(omnetpp::cMessage* msg) {
    if (msg == stepTimer_) {
        core_.applyEvent(nextEventIdx_);
        ++nextEventIdx_;
        scheduleNextTraceEvent();
    }
}

void TraceEventPlayer::finish() {
    const PlayerSummary& s = core_.summary();
    recordScalar("totalCryptoBytes", static_cast<double>(s.totalCryptoBytes));
    recordScalar("totalHopBytes", static_cast<double>(s.totalHopBytes));
    recordScalar("rekeyEvents", static_cast<double>(s.rekeyEvents));
    recordScalar("collateralBatches", static_cast<double>(s.collateralBatches));
    recordScalar("collateralLeavesBatched", static_cast<double>(s.collateralLeavesBatched));
    recordScalar("totalLeaderTimeMs", s.totalLeaderTimeMs);
    recordScalar("activeUavsEnd", static_cast<double>(s.activeUavsEnd));
}

#endif // OMNETPP_STANDALONE_TEST

} // namespace dthsbp
