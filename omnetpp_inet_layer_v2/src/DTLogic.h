#ifndef DT_HSBP_V2_DT_LOGIC_H
#define DT_HSBP_V2_DT_LOGIC_H

#include "V2Payloads.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dthsbp_v2 {

enum class NodeRole : uint8_t {
    FOLLOWER     = 0,
    CLUSTER_HEAD = 1,
    SWARM_LEADER = 2
};

inline ProtoKind parseProtoKind(const std::string& name) {
    if (name == "FlatSBP") return ProtoKind::FLAT_SBP;
    if (name == "HSBP") return ProtoKind::HSBP;
    if (name == "DTHSBP") return ProtoKind::DTHSBP;
    if (name == "LKH") return ProtoKind::LKH;
    if (name == "TGDH") return ProtoKind::TGDH;
    return ProtoKind::DTHSBP;
}

inline const char* protoKindName(ProtoKind kind) {
    switch (kind) {
        case ProtoKind::FLAT_SBP: return "FlatSBP";
        case ProtoKind::HSBP:     return "HSBP";
        case ProtoKind::DTHSBP:   return "DTHSBP";
        case ProtoKind::LKH:      return "LKH";
        case ProtoKind::TGDH:     return "TGDH";
        default:                  return "DTHSBP";
    }
}

struct NodeDTState {
    bool initialized = false;
    double lastReportTimeSec = 0.0;
    double posX = 0.0;
    double posY = 0.0;
    double velX = 0.0;
    double velY = 0.0;
    double energyPct = 100.0;
    double linkQ = 1.0;
    double dtScore = 0.2;
    uint64_t reportedEpoch = 0;
};

/**
 * Linear constant-velocity Kalman innovation tracker running inside each CH / SL DTServer.
 * Computes residual score D_i(t) from received Heartbeat+DT reports without oracle knowledge.
 */
class DTKalmanTracker {
public:
    explicit DTKalmanTracker(
        double sigmaPosM = 14.0,
        double sigmaVelMs = 10.0,
        double ewmaAlpha = 0.70
    ) : sigmaPosM_(sigmaPosM), sigmaVelMs_(sigmaVelMs), ewmaAlpha_(ewmaAlpha) {}

    double ingestReport(
        int uavId,
        double timeSec,
        double posX,
        double posY,
        double velX,
        double velY,
        double energyPct,
        double linkQ,
        uint64_t reportedEpoch
    ) {
        NodeDTState& st = states_[uavId];
        if (!st.initialized) {
            st.initialized = true;
            st.lastReportTimeSec = timeSec;
            st.posX = posX;
            st.posY = posY;
            st.velX = velX;
            st.velY = velY;
            st.energyPct = energyPct;
            st.linkQ = linkQ;
            st.reportedEpoch = reportedEpoch;
            st.dtScore = 0.2;
            return st.dtScore;
        }

        const double dt = std::max(0.05, timeSec - st.lastReportTimeSec);
        const double predX = st.posX + st.velX * dt;
        const double predY = st.posY + st.velY * dt;
        const double posErrM = std::hypot(posX - predX, posY - predY);
        const double velDeltaMs = std::hypot(velX - st.velX, velY - st.velY);
        const double eDrop = std::max(0.0, st.energyPct - energyPct);

        // Residual contributions aligned with DT-HSBP/config.m unit weights
        const double posTerm = posErrM / (sigmaPosM_ * std::sqrt(dt));
        const double velTerm = velDeltaMs / sigmaVelMs_;
        const double energySudden = std::max(0.0, (eDrop - 0.5) / 1.5);
        const double energyLow = (energyPct < 15.0) ? ((15.0 - energyPct) / 5.0) : 0.0;
        const double energyTerm = energySudden + energyLow;
        const double linkTerm = (linkQ < 0.65) ? ((0.65 - linkQ) / 0.15) : 0.0;

        const double rawScore = posTerm + velTerm + energyTerm + linkTerm;
        st.dtScore = (1.0 - ewmaAlpha_) * st.dtScore + ewmaAlpha_ * rawScore;

        st.lastReportTimeSec = timeSec;
        st.posX = posX;
        st.posY = posY;
        st.velX = velX;
        st.velY = velY;
        st.energyPct = energyPct;
        st.linkQ = linkQ;
        st.reportedEpoch = reportedEpoch;
        return st.dtScore;
    }

    bool hasNode(int uavId) const {
        return states_.find(uavId) != states_.end();
    }

    const NodeDTState* getState(int uavId) const {
        auto it = states_.find(uavId);
        return (it != states_.end()) ? &it->second : nullptr;
    }

    void removeNode(int uavId) {
        states_.erase(uavId);
    }

    void clear() {
        states_.clear();
    }

    std::vector<int> staleNodes(double nowSec, double timeoutSec) const {
        std::vector<int> out;
        for (const auto& kv : states_) {
            if (kv.second.initialized && (nowSec - kv.second.lastReportTimeSec) >= timeoutSec) {
                out.push_back(kv.first);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    double sigmaPosM_;
    double sigmaVelMs_;
    double ewmaAlpha_;
    std::unordered_map<int, NodeDTState> states_;
};

struct ChainMember {
    int uavId = -1;
    double energyPct = 100.0;
    double dtScore = 0.0;
};

/**
 * Lifespan-Aware Diffie-Hellman Chain Sorting (DT-HSBP Contribution A).
 * Orders members descending by remaining mission lifespan proxy so high-risk
 * / departing nodes sit at the chain tail, enabling O(1) tail truncation.
 */
class LifespanChainOrder {
public:
    static double lifespanMetric(double energyPct, double dtScore, double wEnergy = 1.0, double wStab = 1.0) {
        return wEnergy * (energyPct / 100.0) - wStab * (dtScore / 5.0);
    }

    static std::vector<int> sortDescending(std::vector<ChainMember> members, double wEnergy = 1.0, double wStab = 1.0) {
        std::sort(members.begin(), members.end(), [wEnergy, wStab](const ChainMember& a, const ChainMember& b) {
            const double ma = lifespanMetric(a.energyPct, a.dtScore, wEnergy, wStab);
            const double mb = lifespanMetric(b.energyPct, b.dtScore, wEnergy, wStab);
            if (std::abs(ma - mb) > 1e-9) {
                return ma > mb; // longest remaining lifespan first
            }
            return a.uavId < b.uavId;
        });
        std::vector<int> ids;
        ids.reserve(members.size());
        for (const auto& m : members) {
            ids.push_back(m.uavId);
        }
        return ids;
    }

    /**
     * Evaluate whether all targetIds sit in a contiguous suffix at the tail of sortedOrder.
     * Sets outSuffixSurvivors to the number of surviving members below/after the earliest
     * departed index (0 when all departing nodes occupy the exact tail -> O(1) truncation).
     */
    static bool isContiguousTail(
        const std::vector<int>& sortedOrder,
        const std::vector<int>& targetIds,
        int& outSuffixSurvivors
    ) {
        outSuffixSurvivors = 0;
        if (sortedOrder.empty() || targetIds.empty()) {
            return true;
        }
        std::unordered_set<int> targetSet(targetIds.begin(), targetIds.end());
        int earliestIdx = static_cast<int>(sortedOrder.size());
        int foundCount = 0;
        for (int i = 0; i < static_cast<int>(sortedOrder.size()); ++i) {
            if (targetSet.count(sortedOrder[static_cast<size_t>(i)]) > 0) {
                earliestIdx = std::min(earliestIdx, i);
                ++foundCount;
            }
        }
        if (foundCount == 0) {
            outSuffixSurvivors = 1;
            return true;
        }
        int survivorsAfter = 0;
        for (int i = earliestIdx; i < static_cast<int>(sortedOrder.size()); ++i) {
            if (targetSet.count(sortedOrder[static_cast<size_t>(i)]) == 0) {
                ++survivorsAfter;
            }
        }
        outSuffixSurvivors = survivorsAfter;
        return survivorsAfter == 0;
    }
};

inline int computeTreeHeight(int swarmSize) {
    int n = std::max(2, swarmSize);
    int h = 0;
    int cap = 1;
    while (cap < n) {
        cap <<= 1;
        ++h;
    }
    return h;
}

inline int computeRekeyPayloadBytes(
    ProtoKind proto,
    int activeSwarmSize,
    int clusterSize,
    int excludedCount,
    int suffixSurvivors,
    bool isTailTruncated
) {
    const int nSwarm = std::max(2, activeSwarmSize);
    const int nClust = std::max(2, clusterSize);
    switch (proto) {
        case ProtoKind::DTHSBP: {
            if (isTailTruncated) {
                return 48; // O(1) tail truncation: constant compact key update
            }
            const int updated = std::max(1, std::min(nClust - 1, suffixSurvivors));
            return 48 + 32 * updated;
        }
        case ProtoKind::HSBP: {
            // Unsorted cluster DH chain: average half-chain recomputation
            const int updated = std::max(1, nClust / 2);
            return 48 + 32 * updated;
        }
        case ProtoKind::LKH: {
            const int h = computeTreeHeight(nSwarm);
            return 48 + 32 * (2 * h);
        }
        case ProtoKind::TGDH: {
            const int h = computeTreeHeight(nSwarm);
            return 48 + 32 * (2 * h);
        }
        case ProtoKind::FLAT_SBP: {
            const int remaining = std::max(1, nSwarm - std::max(1, excludedCount));
            return 48 + 32 * remaining;
        }
        default:
            return 48;
    }
}

inline double computeCryptoLeaderDelaySec(
    ProtoKind proto,
    int activeSwarmSize,
    int clusterSize,
    int suffixSurvivors,
    bool isDtPrecomputedHit
) {
    const int nSwarm = std::max(2, activeSwarmSize);
    const int nClust = std::max(2, clusterSize);
    switch (proto) {
        case ProtoKind::DTHSBP:
            // Contribution B: zero transition crypto stall when precomputed by DTServer
            return isDtPrecomputedHit ? 0.0 : (0.00010 * std::max(1, suffixSurvivors));
        case ProtoKind::HSBP:
            return 0.00012 * std::max(1, nClust / 2);
        case ProtoKind::LKH:
            return 0.00008 * computeTreeHeight(nSwarm);
        case ProtoKind::TGDH:
            return 0.00015 * 2 * computeTreeHeight(nSwarm);
        case ProtoKind::FLAT_SBP:
            return 0.00010 * nSwarm;
        default:
            return 0.0;
    }
}

/**
 * Bounded deduplication cache for decentralized counter-suppressed relay flooding.
 */
class SeenPacketCache {
public:
    explicit SeenPacketCache(size_t maxEntries = 4096) : maxEntries_(maxEntries) {}

    bool insertIfNew(int32_t originNodeId, uint32_t seqId, int16_t fragIdx) {
        const uint64_t key = makeKey(originNodeId, seqId, fragIdx);
        if (seen_.find(key) != seen_.end()) {
            return false;
        }
        seen_.insert(key);
        fifo_.push_back(key);
        if (fifo_.size() > maxEntries_) {
            const uint64_t oldest = fifo_.front();
            fifo_.pop_front();
            seen_.erase(oldest);
        }
        return true;
    }

    void clear() {
        seen_.clear();
        fifo_.clear();
    }

private:
    static uint64_t makeKey(int32_t origin, uint32_t seq, int16_t frag) {
        uint64_t o = static_cast<uint32_t>(origin) & 0xFFFFULL;
        uint64_t s = static_cast<uint64_t>(seq) & 0xFFFFFFFFULL;
        uint64_t f = static_cast<uint16_t>(frag) & 0xFFFFULL;
        return (o << 48) | (s << 16) | f;
    }

    size_t maxEntries_;
    std::unordered_set<uint64_t> seen_;
    std::deque<uint64_t> fifo_;
};

} // namespace dthsbp_v2

#endif // DT_HSBP_V2_DT_LOGIC_H
