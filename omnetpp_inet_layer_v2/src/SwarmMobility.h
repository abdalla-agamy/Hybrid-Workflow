#ifndef DT_HSBP_V2_SWARM_MOBILITY_H
#define DT_HSBP_V2_SWARM_MOBILITY_H

#include "inet/mobility/static/StationaryMobility.h"

namespace dthsbp_v2 {

class SwarmMobility : public inet::StationaryMobility {
public:
    virtual ~SwarmMobility() override;

    void activateAt(
        double x,
        double y,
        double homeX,
        double homeY,
        double homeRadiusM,
        double headingRad,
        double speed,
        double energyPct
    );
    void parkOut();
    void injectDisturbance(double dx, double dy, double dvx, double dvy, double dEnergyPct);

    bool isActive() const { return active_; }
    double getEnergyPct() const { return energyPct_; }
    inet::Coord getPositionNow() const { return lastPosition; }
    inet::Coord getVelocityNow() const { return velocity_; }

    virtual const inet::Coord& getCurrentVelocity() override { return velocity_; }
    virtual double getMaxSpeed() const override { return active_ ? maxSpeed_ : 0.0; }

protected:
    virtual void initialize(int stage) override;
    virtual void handleSelfMessage(inet::cMessage* message) override;

private:
    double updateIntervalSec_ = 0.1;
    double minSpeed_ = 4.0;
    double maxSpeed_ = 12.0;
    double playgroundSizeX_ = 1000.0;
    double playgroundSizeY_ = 1000.0;
    double parkX_ = -5000.0;
    double parkY_ = -5000.0;

    bool active_ = false;
    bool disturbed_ = false;
    double homeX_ = 500.0;
    double homeY_ = 500.0;
    double homeRadiusM_ = 110.0;
    double headingRad_ = 0.0;
    double speed_ = 8.0;
    double driftVx_ = 0.0;
    double driftVy_ = 0.0;
    double energyPct_ = 100.0;
    double energyDrainPerSec_ = 0.10;

    inet::Coord velocity_ = inet::Coord::ZERO;
    inet::cMessage* moveTimer_ = nullptr;
};

} // namespace dthsbp_v2

#endif // DT_HSBP_V2_SWARM_MOBILITY_H
