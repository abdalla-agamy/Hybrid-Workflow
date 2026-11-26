package dt_hsbp_omnet_v2.src;

//
// Physical-world scenario orchestrator and passive closed-loop security/network auditor.
// Drives physical UAV spawns, cluster-bounded flight, precursor physical disturbances,
// crashes, planned departures, and Swarm Leader failover WITHOUT leaking oracle events
// to the protocol agents (DTHSBPNodeApp must detect everything over the air).
//
simple SwarmScenarioManager
{
    parameters:
        @class(dthsbp_v2::SwarmScenarioManager);
        string protocol = default("DTHSBP");
        int initialSwarmSize = default(50);
        int numClusters = default(5);
        int appPort = default(5000);
        int mtuBytes = default(1500);
        bool qosEnabled = default(true);
        double ricianKDb = default(4.0);
        double simDurationSec = default(45.0);
        double joinRatePerSec = default(0.25);
        double leaveRatePerSec = default(0.20);
        double failureRatePerSec = default(0.10);
        double disturbanceRatePerSec = default(0.10);
        double slFailoverTimeSec = default(25.0);
        double precursorLeadSec = default(2.3);
        double dtReportIntervalSec = default(1.0);
        double chSummaryIntervalSec = default(5.0);
        double c2BroadcastIntervalSec = default(0.2);
        double missedHeartbeatTimeoutSec = default(3.0);
        double thetaLeave = default(2.5);
        double thetaJoin = default(1.0);
        double collateralLookaheadSec = default(1.5);
        double relayJitterMaxMs = default(2.0);
        double playgroundSizeX @unit(m) = default(1000m);
        double playgroundSizeY @unit(m) = default(1000m);
        double chRingRadiusM @unit(m) = default(140m);
        double clusterRadiusM @unit(m) = default(100m);
}
