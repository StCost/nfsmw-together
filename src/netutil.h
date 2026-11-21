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
