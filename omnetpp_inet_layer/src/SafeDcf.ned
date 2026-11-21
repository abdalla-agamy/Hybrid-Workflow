package dt_hsbp_omnet.src;

import inet.linklayer.ieee80211.mac.coordinationfunction.Dcf;

//
// Subclass of INET 4.7.0 Dcf fixing an upstream ACK-window deadlock where
// dropping a third-party frame unconditionally cancelled startRxTimer.
//
module SafeDcf extends Dcf
{
    parameters:
        @class(dthsbp::SafeDcf);
}
