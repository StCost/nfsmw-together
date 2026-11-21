#pragma once
// Copies of the other game's cars on the ground (tests\test_ground.cpp). traffic.inl measures the
// ground under each copy and asks these how far to move it.
//  The owner's game runs a car it has no world collision under in its cheap "emulated" mode: its height
//  then comes from the road path, not the road surface, so it floats a little (or sinks). The owner marks
//  such cars (TrafficEntry flag 8) and our copy is put at our ground height + the car's ride height
//  (body centre above the ground), learned from the samples where the owner drove it with full physics.
#include <cmath>

namespace Ground {
    constexpr float MinRide = 0.05f, MaxRide = 2.5f;   // a plausible ride height (m): cars ~0.7, trucks ~1.8
    constexpr float MaxFix = 1.5f;                      // never move a copy more than this (another deck, a jump)
    constexpr float FixSpeed = 1.5f;                    // m/s: how fast the correction follows (no popping)

    // A full-physics sample of the owner: learn the ride height (exponential average), when the car is
    // plausibly standing on that ground (not jumping, not on another deck).
    inline bool Learn(float carY, float groundY, float velY, float& ride, bool& known) {
        float h = carY - groundY;
        if (h < MinRide || h > MaxRide || std::fabs(velY) > 1.0f) return false;
        ride = known ? ride + (h - ride) * 0.2f : h;
        known = true;
        return true;
    }
    // The correction a floating copy needs (0 when we can't tell or it would be a big jump).
    inline float Target(bool floating, bool haveGround, float carY, float groundY, bool rideKnown, float ride) {
        if (!floating || !haveGround || !rideKnown) return 0.f;
        float t = groundY + ride - carY;
        return std::fabs(t) > MaxFix ? 0.f : t;
    }
    // Move the current correction towards the target, at most FixSpeed * dt.
    inline float Step(float cur, float target, float dt) {
        if (dt < 0) dt = 0; if (dt > 0.1f) dt = 0.1f;
        float d = target - cur, m = FixSpeed * dt;
        return std::fabs(d) <= m ? target : cur + (d > 0 ? m : -m);
    }
}
