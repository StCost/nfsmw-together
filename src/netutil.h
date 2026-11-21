#pragma once
// Pure helpers shared by the network and game layers. Nothing here touches the game or the socket,
// so the test suite (tests\) covers all of it without the game.
#include <cstdint>
#include <cstring>
#include <cmath>
#include "net.h"

namespace NetUtil {
    // ---- names ------------------------------------------------------------------------------
    // The HUD standings copy racer names into 16-byte slots: 15 characters + NUL.
    constexpr int HUD_NAME_SIZE = 16;
    inline void CopyHudName(char* dst /*[HUD_NAME_SIZE] or larger*/, const char* src) {
        int i = 0;
        if (src) for (; i < HUD_NAME_SIZE - 1 && src[i]; ++i) dst[i] = src[i];
        dst[i] = 0;
    }

    // ---- traffic ids ------------------------------------------------------------------------
    // Each side numbers the cars it owns 1..0x7FFF. On the wire the client's ids carry bit 15, so
    // both sides can tell whose car an entry is (and never mirror their own cars back).
    constexpr uint16_t CLIENT_ID_BIT = 0x8000;
    inline uint16_t TrafficWireId(uint16_t localId, bool senderIsHost) {
        return senderIsHost ? localId : (uint16_t)(localId | CLIENT_ID_BIT);
    }
    inline bool TrafficIdIsClients(uint16_t wireId) { return (wireId & CLIENT_ID_BIT) != 0; }
    // the host accepts only the client's cars, the client only the host's
    inline bool TrafficIdAccepted(uint16_t wireId, bool weAreHost) { return weAreHost == TrafficIdIsClients(wireId); }
    // next local id after `id` (wraps to 1, never 0 and never with the client bit)
    inline uint16_t NextTrafficLocalId(uint16_t id) {
        ++id;
        return (id == 0 || id >= CLIENT_ID_BIT) ? (uint16_t)1 : id;
    }

    // ---- shared traffic inside races -----------------------------------------------------------
    // In a race the client never deletes its own traffic (the race scripts own some of those cars).
    // Instead each host traffic car is bound to one of the client's own traffic cars, which then
    // follows the host's car. A candidate is one of the client's own traffic cars; `skip` marks the
    // ones that may not be taken (already bound, too close to a player, ...).
    struct TrafficCandidate { uint32_t model; float pos[3]; bool skip; };
    // Index of the best partner for a host car of `model` at `pos`: a car of the same model first
    // (nearest of those), otherwise the nearest car of any model when `anyModel`. -1 if none.
    inline int PickTrafficPartner(uint32_t model, const float* pos, const TrafficCandidate* c, int n, bool anyModel) {
        int best = -1, bestAny = -1;
        float bestD = 0, bestAnyD = 0;
        for (int i = 0; i < n; ++i) {
            if (c[i].skip) continue;
            float dx = c[i].pos[0] - pos[0], dy = c[i].pos[1] - pos[1], dz = c[i].pos[2] - pos[2];
            float d = dx * dx + dy * dy + dz * dz;
            if (!(d == d) || d > 150.f * 150.f) continue;                      // NaN, or too far to jump in unnoticed
            if (c[i].model == model && (best < 0 || d < bestD)) { best = i; bestD = d; }
            if (bestAny < 0 || d < bestAnyD) { bestAny = i; bestAnyD = d; }
        }
        return best >= 0 ? best : (anyModel ? bestAny : -1);
    }

    // ---- smooth traffic handover (client, free roam) ------------------------------------------
    // When the client comes near the host, the host's cars may only appear where the player can't see
    // it happen, and the client's own cars only go once they are off screen. The camera is a view
    // cone in game coordinates (y up); `valid` false = the camera is unknown (then pos is the player).
    struct CamView { float pos[3]; float dir[3]; bool valid; };
    constexpr float HANDOVER_VIEW_COS   = 0.342f;  // cos 70 deg: the cone counted as "in view" (wider than the game's FOV)
    constexpr float HANDOVER_FAR_M      = 180.f;   // farther from the camera than this a car may appear even in view
    constexpr float HANDOVER_NEAR_M     = 30.f;    // never appear closer to the player than this (look-back, mirror)
    constexpr float HANDOVER_CAR_M      = 3.5f;    // a car's radius: a car half inside the cone is in view
    constexpr float HANDOVER_OVERLAP_M  = 6.f;     // two cars closer than this would sit inside each other
    constexpr float HANDOVER_GONE_M     = 500.f;   // beyond this our own car may go even if (a few pixels) on screen
    constexpr float HANDOVER_OFFSCREEN_S = 0.3f;   // off screen at least this long before a car is removed

    inline float Dist3(const float* a, const float* b) {
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return sqrtf(dx * dx + dy * dy + dz * dz);
    }
    // Could the camera see a car at p? Only within farM; an unknown camera sees everything within farM.
    inline bool InCameraView(const CamView& c, const float* p, float farM) {
        float d[3] = { p[0] - c.pos[0], p[1] - c.pos[1], p[2] - c.pos[2] };
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(dist == dist)) return true;                       // NaN: assume seen
        if (dist > farM) return false;
        if (!c.valid || dist <= HANDOVER_CAR_M) return true;
        float dl = sqrtf(c.dir[0] * c.dir[0] + c.dir[1] * c.dir[1] + c.dir[2] * c.dir[2]);
        if (!(dl > 1e-3f)) return true;                         // no direction: assume seen
        float cosA = (d[0] * c.dir[0] + d[1] * c.dir[1] + d[2] * c.dir[2]) / (dist * dl);
        cosA = cosA > 1.f ? 1.f : cosA < -1.f ? -1.f : cosA;
        float margin = asinf(HANDOVER_CAR_M / dist);           // the car's own size widens the cone
        return acosf(cosA) <= acosf(HANDOVER_VIEW_COS) + margin;
    }
    // May a host car be created at p now without the player seeing it pop in: not right next to the
    // player, and outside the camera's view or far away.
    inline bool MaySpawnUnseen(const CamView& c, const float* p, const float* player) {
        if (player && !(Dist3(p, player) >= HANDOVER_NEAR_M)) return false;   // (NaN too)
        return !InCameraView(c, p, HANDOVER_FAR_M);
    }
    // May one of the client's own traffic cars be removed? The game's per-car screen timers
    // (IVehicle::GetOnScreenTime / GetOffScreenTime): never while on screen, only once it has been off
    // screen for a moment (both timers 0 = not drawn at all), or when it is far beyond drawing range.
    inline bool OwnCarMayGo(float onScreenS, float offScreenS, float distToCamM) {
        if (distToCamM >= HANDOVER_GONE_M) return true;
        if (!(onScreenS <= 0.f)) return false;                  // on screen (or garbage): keep
        return offScreenS <= 0.f || offScreenS >= HANDOVER_OFFSCREEN_S;
    }
    // A host car whose spot is in view can take over one of our own cars instead: one of the same model
    // within sameM, else any model within anyM (the car jumps that far: keep it short). -1 if none.
    inline int PickTakeoverPartner(uint32_t model, const float* pos, const TrafficCandidate* c, int n, float sameM, float anyM) {
        int best = -1, bestAny = -1;
        float bestD = 0, bestAnyD = 0;
        for (int i = 0; i < n; ++i) {
            if (c[i].skip) continue;
            float d = Dist3(c[i].pos, pos);
            if (!(d == d)) continue;
            if (c[i].model == model && d <= sameM && (best < 0 || d < bestD)) { best = i; bestD = d; }
            if (d <= anyM && (bestAny < 0 || d < bestAnyD)) { bestAny = i; bestAnyD = d; }
        }
        return best >= 0 ? best : bestAny;
    }

    // ---- no new cars in front of the other player ([Traffic] NoSpawnNearPlayers) ---------------
    // The game picks spots for new traffic cars and cops around its OWN player only, so it happily puts
    // one right in front of the other player, who then crashes into it. A spot is refused when it is
    // close to that player's car and in front of it (their camera looks along the car), in any
    // direction when very close. The car's heading comes as up to two directions (the way the body
    // points, the way it moves); y is up, the cone is measured flat (x, z).
    constexpr float NOSPAWN_RADIUS_M   = 60.f;   // farther than this a new car is fine anywhere
    constexpr float NOSPAWN_ANY_M      = 15.f;   // closer than this: never, whatever the direction
    constexpr float NOSPAWN_VIEW_COS   = 0.5f;   // cos 60 deg: the half-cone counted as "in front"
    constexpr float NOSPAWN_CAM_BACK_M = 6.f;    // the chase camera sits about this far behind the car
    inline bool SpawnSpotBlocked(const float* p, const float* car, const float* dirs, int nDirs) {
        float d = Dist3(p, car);
        if (!(d == d)) return false;                            // NaN spot: not ours to judge
        if (d >= NOSPAWN_RADIUS_M) return false;
        if (d < NOSPAWN_ANY_M) return true;
        bool anyDir = false;
        for (int k = 0; k < nDirs; ++k) {
            const float* f = dirs + 3 * k;
            float fl = sqrtf(f[0] * f[0] + f[2] * f[2]);
            if (!(fl > 1e-3f)) continue;                        // no usable direction (or NaN)
            anyDir = true;
            float fx = f[0] / fl, fz = f[2] / fl;
            // seen from where the camera is (behind the car): the cone also covers the car's flanks
            float vx = p[0] - (car[0] - fx * NOSPAWN_CAM_BACK_M), vz = p[2] - (car[2] - fz * NOSPAWN_CAM_BACK_M);
            float vl = sqrtf(vx * vx + vz * vz);
            if (!(vl > 1e-3f)) return true;
            if ((vx * fx + vz * fz) / vl >= NOSPAWN_VIEW_COS) return true;
        }
        return !anyDir;                                          // heading unknown: the whole circle counts
    }
    // A host car (or the other player's cop) we haven't created here yet may appear now: not inside
    // our camera's view within NOSPAWN_RADIUS_M (else it waits until it is out of view or farther).
    inline bool MirrorMayAppear(const CamView& c, const float* p) {
        return !InCameraView(c, p, NOSPAWN_RADIUS_M);
    }

    // ---- AI catch-up in co-op ----------------------------------------------------------------
    // The race % the AI catch-up measures itself against: the leading player. ourPct < 0 = we have no
    // racer (leave the game alone: false). The partner counts while `peerOk`.
    inline bool CatchUpLeaderPct(float ourPct, bool peerOk, float peerPct, float& out) {
        if (!(ourPct >= 0.f)) return false;
        out = ourPct;
        if (peerOk && peerPct == peerPct && peerPct > out) out = peerPct > 100.f ? 100.f : peerPct;
        return true;
    }

    // ---- interpolation math -----------------------------------------------------------------
    inline void Lerp3(const float* a, const float* b, float t, float* o) {
        for (int i = 0; i < 3; ++i) o[i] = a[i] + (b[i] - a[i]) * t;
    }

    // Normalized lerp of two quaternions (x,y,z,w), taking the short way round.
    inline void NlerpQ(const float* a, const float* b0, float t, float* o) {
        float b[4] = { b0[0], b0[1], b0[2], b0[3] };
        if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0) for (float& x : b) x = -x;
        float len = 0;
        for (int i = 0; i < 4; ++i) { o[i] = a[i] + (b[i] - a[i]) * t; len += o[i] * o[i]; }
        len = sqrtf(len);
        if (len > 1e-6f) for (int i = 0; i < 4; ++i) o[i] /= len;
    }

    // Cubic Hermite between (p0, v0) and (p1, v1), `dt` seconds apart, at u in [0,1].
    inline float Hermite(float p0, float v0, float p1, float v1, float dt, float u) {
        float u2 = u * u, u3 = u2 * u;
        float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        return h00 * p0 + h10 * dt * v0 + h01 * p1 + h11 * dt * v1;
    }

    // ---- race standings ------------------------------------------------------------------------
    // The other player's race % (GRacerInfo +0x18, 0..100 over the whole race) as it is at sender time
    // `now`, from two of their samples (pctOld at tOld, pctNew at tNew, ms in their clock). The newest
    // value is moved on at the rate between the two samples, for at most `maxAheadMs` (late packets),
    // never backwards (a restart drops the % to 0: no negative rate) and never past 100.
    inline float RacePctAt(float pctOld, double tOld, float pctNew, double tNew, double now, double maxAheadMs = 250.0) {
        if (!(pctNew == pctNew)) return 0.f;
        float out = pctNew;
        double span = tNew - tOld, ahead = now - tNew;
        if (span >= 20.0 && ahead > 0.0 && pctOld == pctOld) {
            double rate = (pctNew - pctOld) / span;              // % per ms
            if (rate > 0.0 && rate < 0.05) {                     // 0.05 %/ms = a whole race in 2 s: anything faster is a jump
                if (ahead > maxAheadMs) ahead = maxAheadMs;
                out = (float)(pctNew + rate * ahead);
            }
        }
        if (out > 100.f) out = 100.f;
        if (out < 0.f) out = 0.f;
        return out;
    }

    // Grid slot sharing at a race start. Both games put their own player on the player's grid slot,
    // so the two players stand on the same spot; the client moves its car to the slot its game built
    // the host's racer on (where the host's game has the client's racer), so the two cars are apart
    // on both screens from the first frame. True when that move is due: both cars stand still on the
    // same spot (< 3 m) and the other player's slot is a nearby, different one (2..40 m away).
    inline bool ShouldTakePeerSlot(const float* myPos, float mySpeed, const float* peerPos, float peerSpeed, const float* slotPos) {
        auto d = [](const float* a, const float* b) {
            float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return sqrtf(dx * dx + dy * dy + dz * dz);
        };
        auto ok = [](const float* v) { for (int i = 0; i < 3; ++i) if (!(fabsf(v[i]) <= 1.0e6f)) return false; return true; };
        if (!ok(myPos) || !ok(peerPos) || !ok(slotPos)) return false;
        if (!(mySpeed < 1.f) || !(peerSpeed < 1.f)) return false;      // the countdown: nobody is moving yet
        float slot = d(myPos, slotPos);
        return d(myPos, peerPos) < 3.f && slot > 2.f && slot < 40.f;
    }

    // ---- validation of what the network hands us ----------------------------------------------
    // These floats go straight into the game's rigid bodies: a NaN / infinity / absurd value from a
    // broken or hostile peer must never get that far.
    inline bool FiniteWithin(const float* v, int n, float limit) {
        for (int i = 0; i < n; ++i) if (!(fabsf(v[i]) <= limit)) return false;   // false for NaN too
        return true;
    }
    constexpr float MAX_POS = 1.0e6f, MAX_VEL = 1.0e4f, MAX_SIMTIME_S = 1.0e9f;
    inline bool ValidMotion(const float* pos, const float* rot, const float* linVel, const float* angVel, bool realRotation = true) {
        if (!FiniteWithin(pos, 3, MAX_POS) || !FiniteWithin(rot, 4, 2.0f) ||
            !FiniteWithin(linVel, 3, MAX_VEL) || !FiniteWithin(angVel, 3, MAX_VEL)) return false;
        float q = rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3];
        return !realRotation || (q > 0.5f && q < 1.5f);          // a real rotation (a zero quaternion would stay zero after nlerp)
    }
    inline bool ValidCarState(const CarState& s) {
        return ValidMotion(s.pos, s.rot, s.linVel, s.angVel, s.hasCar != 0) &&   // no car: all zeros is fine
               FiniteWithin(&s.speedMps, 1, MAX_VEL) && FiniteWithin(&s.simTime, 1, MAX_SIMTIME_S) &&
               FiniteWithin(&s.steer, 1, 1.5f) &&
               FiniteWithin(&s.racePct, 1, 101.f) && s.racePct >= -1.f &&
               FiniteWithin(&s.lapPct, 1, 101.f) && s.lapPct >= -1.f &&
               s.laps >= 0 && s.laps <= 200 && s.checkpoints >= 0 && s.checkpoints <= 2000 &&
               s.heat100 <= MAX_HEAT100 && s.coolLeft10 <= MAX_COOL_LEFT10 && !(s.pursuitFlags & ~PURSUIT_KNOWN_BITS);
    }
    inline bool ValidTrafficEntry(const TrafficEntry& e) {
        return ValidMotion(e.pos, e.rot, e.linVel, e.angVel);
    }
}
