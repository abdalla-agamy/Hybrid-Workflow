package dt_hsbp_omnet.simulations;

import dt_hsbp_omnet.src.DTHSBPRekeyApp;
import dt_hsbp_omnet.src.TraceEventPlayer;
import dt_hsbp_omnet.src.TraceMobility;
import inet.networklayer.configurator.ipv4.Ipv4NetworkConfigurator;
import inet.node.inet.AdhocHost;
import inet.physicallayer.wireless.ieee80211.packetlevel.Ieee80211ScalarRadioMedium;

//
// UAV Ad-Hoc host extending INET's AdhocHost with bounded carry-forward TraceMobility
// and UDP/802.11 DTHSBPRekeyApp.
//
module UAVNode extends AdhocHost
{
    parameters:
        mobility.typename = default("dt_hsbp_omnet.src.TraceMobility");
        numApps = default(1);
        app[0].typename = default("dt_hsbp_omnet.src.DTHSBPRekeyApp");
}

//
// Primary OMNeT++ / INET 4.7.0 simulation network for DT-HSBP Phase 5 replay.
//
network DTHSBPSwarmNetwork
{
    parameters:
        int numUavs = default(70);
        double playgroundSizeX @unit(m) = default(1000m);
        double playgroundSizeY @unit(m) = default(1000m);
        @display("bgb=1000,1000");

    submodules:
        configurator: Ipv4NetworkConfigurator {
            @display("p=50,60");
        }
        radioMedium: Ieee80211ScalarRadioMedium {
            @display("p=50,140");
        }
        tracePlayer: TraceEventPlayer {
            @display("p=50,220");
        }
        uav[numUavs]: UAVNode {
            @display("p=500,500");
        }
}
