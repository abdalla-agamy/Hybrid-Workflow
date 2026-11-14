package dt_hsbp_omnet.src;

//
// Simple module that reads a deterministic MATLAB JSON trace, maintains full-swarm
// 2D node positions via bounded carry-forward, and injects rekey traffic into INET hosts.
//
simple TraceEventPlayer
{
    parameters:
        string traceFile;
        bool enableMultiHopForwarding = default(true);
        int mtuBytes = default(1500);
        double commRangeM @unit(m) = default(350m);
        @display("i=block/cogwheel");
}
