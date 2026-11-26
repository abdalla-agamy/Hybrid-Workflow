package dt_hsbp_omnet_v2.simulations;

import dt_hsbp_omnet_v2.src.DTHSBPNodeApp;
import dt_hsbp_omnet_v2.src.SwarmMobility;
import dt_hsbp_omnet_v2.src.SwarmScenarioManager;
import inet.networklayer.configurator.ipv4.Ipv4NetworkConfigurator;
import inet.node.inet.AdhocHost;
import inet.physicallayer.wireless.ieee80211.packetlevel.Ieee80211ScalarRadioMedium;

module UAVNodeV2 extends AdhocHost
{
    parameters:
        mobility.typename = default("dt_hsbp_omnet_v2.src.SwarmMobility");
        numApps = default(1);
        app[0].typename = default("dt_hsbp_omnet_v2.src.DTHSBPNodeApp");
}

network DTHSBPV2SwarmNetwork
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
        scenarioManager: SwarmScenarioManager {
            @display("p=50,220");
        }
        uav[numUavs]: UAVNodeV2 {
            @display("p=500,500");
        }
}
