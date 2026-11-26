package dt_hsbp_omnet_v2.src;

import inet.mobility.static.StationaryMobility;

//
// Continuous bounded 2D mobility module for DT-HSBP v2 active UAVs.
// Parked/inactive slots sit stationary at (parkX, parkY) until activated by
// SwarmScenarioManager. Supports smooth cluster-bounded cruise and physical
// kinematic/energy disturbance injection for closed-loop DTServer detection.
//
simple SwarmMobility extends StationaryMobility
{
    parameters:
        @class(dthsbp_v2::SwarmMobility);
        double updateInterval @unit(s) = default(0.1s);
        double minSpeed @unit(mps) = default(4mps);
        double maxSpeed @unit(mps) = default(12mps);
        double playgroundSizeX @unit(m) = default(1000m);
        double playgroundSizeY @unit(m) = default(1000m);
        double parkX @unit(m) = default(-5000m);
        double parkY @unit(m) = default(-5000m);
}
