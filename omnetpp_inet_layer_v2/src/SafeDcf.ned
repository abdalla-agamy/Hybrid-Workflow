package dt_hsbp_omnet_v2.src;

import inet.linklayer.ieee80211.mac.coordinationfunction.Dcf;

//
// Subclass of INET 4.7.0 Dcf fixing the third-party ACK-window startRxTimer
// deadlock for the qosStation=false DCF ablation baseline in v2.
//
module SafeDcf extends Dcf
{
    parameters:
        @class(dthsbp_v2::SafeDcf);
}
