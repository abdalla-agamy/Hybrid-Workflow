#include "TraceEventPlayer.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct ExpectedTraceStats {
    const char* protocol;
    long long expectedCryptoBytes;
    size_t minEvents;
    size_t expectedCollateralBatched;
};

void assertNearZeroToArena(double v, double maxBound, const char* label) {
    assert(std::isfinite(v));
    if (v < -1e-6 || v > maxBound + 1e-6) {
        std::cerr << "Coordinate out of arena bounds for " << label << ": " << v << "\n";
        assert(false);
    }
}

} // namespace

int main() {
    const ExpectedTraceStats specs[5] = {
        {"FlatSBP", 298984LL, 90, 0},
        {"HSBP",    61040LL,  90, 0},
        {"DTHSBP",  58800LL,  90, 4},
        {"LKH",     30768LL,  90, 0},
        {"TGDH",    51072LL,  90, 0}
    };

    const std::string samplesDir = "../traces/samples";

    for (size_t i = 0; i < 5; ++i) {
        const ExpectedTraceStats& spec = specs[i];
        const std::string path = samplesDir + "/events_trace_" + spec.protocol + "_N50.json";

        dthsbp::TracePlayerCore player;
        player.loadFromFile(path);

        const dthsbp::TraceMeta& meta = player.meta();
        assert(meta.protocol == spec.protocol);
        assert(meta.swarmSize == 50);
        assert(meta.numClusters == 5);
        assert(meta.seed == 42);
        assert(meta.mtu == 1500);
        assert(player.countActiveNodes() == 50);
        assert(player.hasValidHeadForAllNonEmptyClusters());
        assert(player.events().size() >= spec.minEvents);

        // Verify bounded nodePositions snapshot invariant & full-swarm carry-forward
        for (size_t eIdx = 0; eIdx < player.events().size(); ++eIdx) {
            player.applyEvent(eIdx);
            const dthsbp::TraceEventRecord& ev = player.events()[eIdx];

            // Bounded snapshot scoping: only target cluster + SL (plus at most joiner/batch)
            assert(ev.nodePositions.size() <= 25);

            // Carry-forward must preserve >= 40 active UAVs across all 5 clusters at all times
            int activeCount = player.countActiveNodes();
            assert(activeCount >= 40 && activeCount <= 90);
            assert(player.hasValidHeadForAllNonEmptyClusters());

            const auto& nodes = player.nodes();
            for (auto it = nodes.begin(); it != nodes.end(); ++it) {
                if (it->second.active) {
                    assertNearZeroToArena(it->second.x, meta.arenaWidthM, "x");
                    assertNearZeroToArena(it->second.y, meta.arenaHeightM, "y");
                }
            }
        }

        const dthsbp::PlayerSummary& s = player.summary();
        assert(s.totalCryptoBytes == spec.expectedCryptoBytes);
        assert(s.collateralLeavesBatched == spec.expectedCollateralBatched);
        assert(s.carryForwardNodesPreserved > 3000);
        assert(s.maxBoundedSnapshotSize <= 25);
        assert(s.totalFragments > 0);

        std::cout << "[PASS] " << spec.protocol
                  << " | events=" << s.numEvents
                  << " | cryptoBytes=" << s.totalCryptoBytes
                  << " | hopBytes=" << s.totalHopBytes
                  << " | collateralBatched=" << s.collateralLeavesBatched
                  << " | maxSnapshot=" << s.maxBoundedSnapshotSize
                  << " | carryForwardUpdates=" << s.carryForwardNodesPreserved
                  << " | activeEnd=" << s.activeUavsEnd
                  << std::endl;
    }

    std::cout << "ALL 5 STANDALONE C++14 TRACE PLAYER TESTS PASSED." << std::endl;
    return 0;
}
