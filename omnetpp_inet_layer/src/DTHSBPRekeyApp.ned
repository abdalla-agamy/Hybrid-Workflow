package dt_hsbp_omnet.src;

import inet.applications.contract.IApp;
import inet.common.SimpleModule;

//
// UDP/IPv4/IEEE 802.11 application module installed on every UAVNode.
// Dispatches trace-driven rekey fragments and reports received frames back
// to TraceEventPlayer for multi-round key-sync and latency verification.
//
simple DTHSBPRekeyApp extends SimpleModule like IApp
{
    parameters:
        @class(dthsbp::DTHSBPRekeyApp);
        int localPort = default(5000);
        double stopOperationExtraTime @unit(s) = default(-1s);
        double stopOperationTimeout @unit(s) = default(2s);
        @display("i=block/app");
        @lifecycleSupport;
        @signal[packetSent](type=inet::Packet);
        @signal[packetReceived](type=inet::Packet);
        @statistic[packetSent](title="rekey fragments sent"; source=packetSent; record=count,"sum(packetBytes)"; interpolationmode=none);
        @statistic[packetReceived](title="rekey fragments received"; source=packetReceived; record=count,"sum(packetBytes)"; interpolationmode=none);

    gates:
        input socketIn @labels(UdpCommand/up);
        output socketOut @labels(UdpCommand/down);
}
