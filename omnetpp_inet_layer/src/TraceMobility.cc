#include "TraceMobility.h"

namespace dthsbp {

Define_Module(TraceMobility);

void TraceMobility::setTracePosition(double x, double y, double z) {
    Enter_Method_Silent();
    lastPosition = inet::Coord(x, y, z);
    emitMobilityStateChangedSignal();
}

} // namespace dthsbp
