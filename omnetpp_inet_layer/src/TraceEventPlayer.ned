package dt_hsbp_omnet.src;

//
// Simple module that reads a deterministic MATLAB JSON trace, maintains full-swarm
// 2D node positions via bounded carry-forward, drives host mobility via TraceMobility,
// and injects real UDP/802.11 rekey traffic across UAVNode hosts in INET 4.7.0.
//
simple TraceEventPlayer
{
    parameters:
        @class(dthsbp::TraceEventPlayer);
        string traceFile;
        string eventLogFile = default("");
        bool enableMultiHopForwarding = default(true);
        int mtuBytes = default(1500);
        int maxRetries = default(3);
        double commRangeM @unit(m) = default(350m);
        double carrierSenseRangeM @unit(m) = default(700m);
        double bitrateMbps = default(6.0);
        double pLossExtra = default(0.0);
        double retryTimeoutMultiplier = default(3.0);
        double relayJitterMaxMs = default(2.0);
        int rngSeed = default(42);
        @display("i=block/cogwheel");
        @signal[rekeyLatency](type=double);
        @signal[eventPdr](type=double);
        @signal[eventKeySync](type=double);
        @statistic[rekeyLatency](title="end-to-end rekey completion latency (ms)"; record=mean,max,vector?; interpolationmode=none);
        @statistic[eventPdr](title="per-event packet delivery ratio"; record=mean; interpolationmode=none);
        @statistic[eventKeySync](title="per-event effective key sync ratio"; record=mean; interpolationmode=none);
}
