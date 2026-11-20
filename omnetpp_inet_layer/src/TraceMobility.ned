package dt_hsbp_omnet.src;

import inet.mobility.static.StationaryMobility;

//
// StationaryMobility subclass whose coordinates are updated programmatically by
// TraceEventPlayer strictly from bounded nodePositions snapshots (carry-forward).
//
simple TraceMobility extends StationaryMobility
{
    parameters:
        @class(dthsbp::TraceMobility);
}
