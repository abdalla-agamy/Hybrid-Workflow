#include "SwarmMobility.h"

#include <algorithm>
#include <cmath>

namespace dthsbp_v2 {

Define_Module(SwarmMobility);

SwarmMobility::~SwarmMobility() {
    cancelAndDelete(moveTimer_);
    moveTimer_ = nullptr;
}

void SwarmMobility::initialize(int stage) {
    StationaryMobility::initialize(stage);
    if (stage == inet::INITSTAGE_LOCAL) {
        updateIntervalSec_ = par("updateInterval").doubleValue();
        minSpeed_ = par("minSpeed").doubleValue();
        maxSpeed_ = par("maxSpeed").doubleValue();
        playgroundSizeX_ = par("playgroundSizeX").doubleValue();
        playgroundSizeY_ = par("playgroundSizeY").doubleValue();
        parkX_ = par("parkX").doubleValue();
        parkY_ = par("parkY").doubleValue();
        moveTimer_ = new inet::cMessage("SwarmMobilityMove");
        active_ = false;
        velocity_ = inet::Coord::ZERO;
    }
}

void SwarmMobility::activateAt(
    double x,
    double y,
    double homeX,
    double homeY,
    double homeRadiusM,
    double headingRad,
    double speed,
    double energyPct
) {
    Enter_Method("activateAt");
    homeX_ = homeX;
    homeY_ = homeY;
    homeRadiusM_ = std::max(20.0, homeRadiusM);
    headingRad_ = headingRad;
    speed_ = std::clamp(speed, minSpeed_, maxSpeed_);
    energyPct_ = std::clamp(energyPct, 5.0, 100.0);
    disturbed_ = false;
    driftVx_ = 0.0;
    driftVy_ = 0.0;
    active_ = true;

    lastPosition = inet::Coord(
        std::clamp(x, 30.0, playgroundSizeX_ - 30.0),
        std::clamp(y, 30.0, playgroundSizeY_ - 30.0),
        0.0
    );
    velocity_ = inet::Coord(speed_ * std::cos(headingRad_), speed_ * std::sin(headingRad_), 0.0);
    emitMobilityStateChangedSignal();

    cancelEvent(moveTimer_);
    if (updateIntervalSec_ > 0.0) {
        scheduleAt(inet::simTime() + updateIntervalSec_, moveTimer_);
    }
}

void SwarmMobility::parkOut() {
    Enter_Method("parkOut");
    active_ = false;
    disturbed_ = false;
    driftVx_ = 0.0;
    driftVy_ = 0.0;
    velocity_ = inet::Coord::ZERO;
    lastPosition = inet::Coord(parkX_, parkY_, 0.0);
    cancelEvent(moveTimer_);
    emitMobilityStateChangedSignal();
}

void SwarmMobility::injectDisturbance(double dx, double dy, double dvx, double dvy, double dEnergyPct) {
    Enter_Method("injectDisturbance");
    if (!active_) {
        return;
    }
    disturbed_ = true;
    driftVx_ = dvx;
    driftVy_ = dvy;
    lastPosition.x = std::clamp(lastPosition.x + dx, 20.0, playgroundSizeX_ - 20.0);
    lastPosition.y = std::clamp(lastPosition.y + dy, 20.0, playgroundSizeY_ - 20.0);
    velocity_.x += dvx;
    velocity_.y += dvy;
    energyPct_ = std::max(1.0, energyPct_ - std::max(0.0, dEnergyPct));
    emitMobilityStateChangedSignal();
}

void SwarmMobility::handleSelfMessage(inet::cMessage* message) {
    if (message != moveTimer_) {
        StationaryMobility::handleSelfMessage(message);
        return;
    }
    if (!active_) {
        return;
    }

    const double dt = std::max(0.01, updateIntervalSec_);
    headingRad_ += uniform(-0.15, 0.15);

    double vx = speed_ * std::cos(headingRad_);
    double vy = speed_ * std::sin(headingRad_);

    if (disturbed_) {
        vx += driftVx_;
        vy += driftVy_;
        energyPct_ = std::max(0.5, energyPct_ - 1.5 * dt);
    } else {
        // Gentle cohesive cluster spring beyond 85% of cluster radius
        const double rx = lastPosition.x - homeX_;
        const double ry = lastPosition.y - homeY_;
        const double dist = std::hypot(rx, ry);
        if (dist > homeRadiusM_ * 0.85 && dist > 1e-3) {
            const double pull = 0.35 * speed_;
            vx -= pull * (rx / dist);
            vy -= pull * (ry / dist);
            headingRad_ = std::atan2(vy, vx);
        }
        energyPct_ = std::max(1.0, energyPct_ - energyDrainPerSec_ * dt);
    }

    double nx = lastPosition.x + vx * dt;
    double ny = lastPosition.y + vy * dt;

    if (nx < 30.0) {
        nx = 30.0;
        vx = std::abs(vx);
        headingRad_ = std::atan2(vy, vx);
    } else if (nx > playgroundSizeX_ - 30.0) {
        nx = playgroundSizeX_ - 30.0;
        vx = -std::abs(vx);
        headingRad_ = std::atan2(vy, vx);
    }
    if (ny < 30.0) {
        ny = 30.0;
        vy = std::abs(vy);
        headingRad_ = std::atan2(vy, vx);
    } else if (ny > playgroundSizeY_ - 30.0) {
        ny = playgroundSizeY_ - 30.0;
        vy = -std::abs(vy);
        headingRad_ = std::atan2(vy, vx);
    }

    lastPosition.x = nx;
    lastPosition.y = ny;
    velocity_ = inet::Coord(vx, vy, 0.0);
    emitMobilityStateChangedSignal();

    scheduleAt(inet::simTime() + updateIntervalSec_, moveTimer_);
}

} // namespace dthsbp_v2
