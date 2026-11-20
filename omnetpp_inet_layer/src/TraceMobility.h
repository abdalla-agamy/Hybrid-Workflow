#ifndef DT_HSBP_TRACE_MOBILITY_H
#define DT_HSBP_TRACE_MOBILITY_H

#include "inet/mobility/static/StationaryMobility.h"

namespace dthsbp {

/**
 * INET mobility module holding static carried-forward 2D coordinates until updated
 * by TraceEventPlayer whenever this UAV appears in a trace event's bounded nodePositions.
 */
class TraceMobility : public inet::StationaryMobility {
public:
    void setTracePosition(double x, double y, double z = 0.0);
};

} // namespace dthsbp

#endif // DT_HSBP_TRACE_MOBILITY_H
