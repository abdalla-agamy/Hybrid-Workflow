package dt_hsbp_omnet.simulations;

import dt_hsbp_omnet.src.TraceEventPlayer;
import inet.networklayer.configurator.ipv4.Ipv4NetworkConfigurator;
import inet.node.inet.AdhocHost;
import inet.physicallayer.wireless.ieee80211.packetlevel.Ieee80211ScalarRadioMedium;
import inet.visualizer.canvas.integrated.IntegratedCanvasVisualizer;

//
// Hybrid Trace-Driven UAV Swarm Network for DT-HSBP Cross-Validation (Phase 5B).
// Node positions, cluster membership transitions, and rekey message bursts are
// driven deterministically from MATLAB-exported swarm_trace_schema.json traces.
//
network SwarmTraceNetwork
{
    parameters:
        int numUavs = default(50);
        double playgroundSizeX @unit(m) = default(1000m);
        double playgroundSizeY @unit(m) = default(1000m);
        @display("bgb=1000,1000");

    submodules:
        visualizer: IntegratedCanvasVisualizer {
            @display("p=50,50");
        }
        configurator: Ipv4NetworkConfigurator {
            @display("p=50,120");
        }
        radioMedium: Ieee80211ScalarRadioMedium {
            @display("p=50,190");
        }
        tracePlayer: TraceEventPlayer {
            @display("p=50,260");
        }
        uav[numUavs]: AdhocHost {
            @display("p=500,500");
        }
}
