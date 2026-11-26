package dt_hsbp_omnet_v2.src;

import inet.applications.contract.IApp;

//
// Closed-loop protocol agent implementing DTAgent (follower 1 Hz Heartbeat + DT report),
// DTServer (CH/SL Kalman residual tracker, Lifespan-Aware Chain Sorting, collateral
// batching, stale-heartbeat watchdog), encrypted 5 Hz C2 receiver/sender,
// counter-suppressed relay forwarder, AC_VO/AC_VI/AC_BE EDCA tagging, and key resync.
//
simple DTHSBPNodeApp like IApp
{
    parameters:
        @class(dthsbp_v2::DTHSBPNodeApp);
        int localPort = default(5000);
        int relaySuppressionThreshold = default(2);
        double ackDeadlineSec = default(0.45);
        double resyncCooldownSec = default(0.80);
        double stopOperationTimeout @unit(s) = default(2s);

        @signal[packetSent](type=inet::Packet);
        @signal[packetReceived](type=inet::Packet);
        @statistic[packetSent](title="packets sent"; source=packetSent; record=count,"sum(packetBytes)","vector(packetBytes)"; interpolationmode=none);
        @statistic[packetReceived](title="packets received"; source=packetReceived; record=count,"sum(packetBytes)","vector(packetBytes)"; interpolationmode=none);

    gates:
        input socketIn @labels(UdpControlInfo/up);
        output socketOut @labels(UdpControlInfo/down);
}
