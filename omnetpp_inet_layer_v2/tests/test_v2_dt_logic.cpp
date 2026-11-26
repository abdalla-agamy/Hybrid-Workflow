#define OMNETPP_STANDALONE_TEST
#include "DTLogic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace dthsbp_v2;

int main() {
    // 1. Verify wire header sizes
    static_assert(sizeof(V2WireHeader) == 48, "V2WireHeader == 48 B");
    static_assert(sizeof(HeartbeatDtPacket) == 64, "HeartbeatDtPacket == 64 B");
    static_assert(sizeof(ChSummaryPacket) == 64, "ChSummaryPacket == 64 B");
    static_assert(sizeof(C2BroadcastPacket) == 128, "C2BroadcastPacket == 128 B");
    static_assert(sizeof(KeyResyncPacket) == 56, "KeyResyncPacket == 56 B");

    // 2. Verify EDCA UP mapping
    assert(userPriorityForMsg(MsgKind::REKEY_BCAST_FRAG) == 6);
    assert(userPriorityForMsg(MsgKind::REKEY_UNICAST_ACK) == 6);
    assert(userPriorityForMsg(MsgKind::KEY_RESYNC_PULL) == 6);
    assert(userPriorityForMsg(MsgKind::MISSION_C2_BCAST) == 4);
    assert(userPriorityForMsg(MsgKind::HEARTBEAT_DT_REPORT) == 0);
    assert(userPriorityForMsg(MsgKind::CH_SUMMARY_TO_SL) == 0);

    // 3. Verify DTKalmanTracker stays nominal on smooth flight and spikes > 2.5 on disturbance
    DTKalmanTracker tracker;
    double s0 = tracker.ingestReport(7, 1.0, 100.0, 200.0, 8.0, 0.0, 99.0, 0.95, 1);
    assert(s0 < 1.0);
    double s1 = tracker.ingestReport(7, 2.0, 108.5, 200.5, 8.2, 0.2, 98.9, 0.94, 1);
    assert(s1 < 1.0);
    // Sudden trajectory jump + speed spike + link drop (precursor disturbance)
    double s2 = tracker.ingestReport(7, 3.0, 145.0, 218.0, 18.0, 6.0, 91.0, 0.50, 1);
    assert(s2 > 2.5);

    auto stale = tracker.staleNodes(6.2, 3.0);
    assert(stale.size() == 1 && stale[0] == 7);

    // 4. Verify LifespanChainOrder places lowest-lifespan / highest-DT-risk at tail for O(1) truncation
    std::vector<ChainMember> members = {
        {10, 95.0, 0.3},
        {11, 50.0, 3.2}, // predicted departing
        {12, 85.0, 0.4},
        {13, 60.0, 2.8}, // collateral predicted departing
    };
    auto sorted = LifespanChainOrder::sortDescending(members);
    assert(sorted[0] == 10);
    assert(sorted[1] == 12);
    int suffixSurvivors = -1;
    bool isTail = LifespanChainOrder::isContiguousTail(sorted, {11, 13}, suffixSurvivors);
    assert(isTail && suffixSurvivors == 0);

    // Non-tail sudden failure of healthy high-lifespan node 10
    int nonTailSurvivors = -1;
    bool notTail = LifespanChainOrder::isContiguousTail(sorted, {10}, nonTailSurvivors);
    assert(!notTail && nonTailSurvivors == 3);

    // 5. Verify protocol byte scaling
    int dtBytes = computeRekeyPayloadBytes(ProtoKind::DTHSBP, 250, 50, 1, 0, true);
    int hsbpBytes = computeRekeyPayloadBytes(ProtoKind::HSBP, 250, 50, 1, 0, false);
    int flatBytes = computeRekeyPayloadBytes(ProtoKind::FLAT_SBP, 250, 50, 1, 0, false);
    assert(dtBytes == 48);
    assert(hsbpBytes > dtBytes);
    assert(flatBytes > 7500);
    assert(computeCryptoLeaderDelaySec(ProtoKind::DTHSBP, 250, 50, 0, true) == 0.0);

    // 6. Verify SeenPacketCache deduplication + bounded FIFO eviction
    SeenPacketCache cache(3);
    assert(cache.insertIfNew(1, 100, 0));
    assert(!cache.insertIfNew(1, 100, 0));
    assert(cache.insertIfNew(1, 100, 1));
    assert(cache.insertIfNew(2, 100, 0));
    assert(cache.insertIfNew(3, 100, 0)); // evicts (1,100,0)
    assert(cache.insertIfNew(1, 100, 0)); // re-insertable after eviction

    std::cout << "test_v2_dt_logic: ALL TESTS PASSED\n";
    return 0;
}
