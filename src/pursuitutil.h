#pragma once
// Shared pursuits ([Coop] SharedPursuits): the decisions that don't need the game, so the test suite
// covers them (tests\test_pursuit.cpp). pursuit.inl reads the game, asks these, and does what they say.
#include <cstdint>
#include "net.h"

namespace SharedPursuit {
    // The game's own limit: a pursuit sees its perpetrator while some cop saw them in the last 7 s
    // (AIPursuit::OnTask compares the time since seen with the float at 0x890858 = 7.0).
    constexpr float SEEN_LIMIT_S = 7.0f;
    constexpr float HEAT_EPS = 0.02f;          // heat travels x100: smaller differences are rounding
    constexpr float DELAY_SLACK_S = 0.25f;     // cooldowns this close count as "together"

    // ---- heat ------------------------------------------------------------------------------------
    // Called with every state from the other player. Returns the heat our car should take, or 0 to
    // leave ours alone. A value is copied only when it is new on their side (first one after a gap, or
    // higher than their last one) and hotter than ours. So when one of you gets heat the other gets the
    // same, the hotter of the two wins, and a heat that drops on one side (busted, another car) is
    // neither copied nor pushed back up by the other side's old value.
    struct HeatTracker { uint16_t lastPeer = 0; bool synced = false; };   // synced: had their first value this session
    inline float HeatToTake(HeatTracker& t, float mine, uint16_t peer100) {
        if (peer100 == 0 || peer100 > MAX_HEAT100) { t.lastPeer = 0; return 0.f; }   // unknown / not in free roam
        // the very first value of the session is taken (both start equal); after a gap (race, shop, menu)
        // the first value is only a baseline, so a heat we lost meanwhile (respray, busted) isn't pushed back
        bool fresh = t.lastPeer ? peer100 > t.lastPeer : !t.synced;
        t.synced = true;
        t.lastPeer = peer100;
        float peer = peer100 / 100.f;
        if (!fresh || !(mine == mine)) return 0.f;
        return peer > mine + HEAT_EPS ? peer : 0.f;
    }
    inline uint16_t HeatToWire(float heat) {
        if (!(heat > 0.f)) return 0;
        float h = heat * 100.f + 0.5f;
        return h >= (float)MAX_HEAT100 ? MAX_HEAT100 : (uint16_t)h;
    }

    // ---- cooldown ----------------------------------------------------------------------------------
    // One side of a pursuit as the other game needs it (CarState::pursuitFlags / coolLeft10).
    struct View { bool inPursuit; bool seen; bool cooling; float left; };
    inline View FromWire(uint8_t flags, uint16_t left10) {
        View v;
        v.inPursuit = (flags & PURSUIT_ACTIVE) != 0;
        v.seen = (flags & PURSUIT_SEEN) != 0;
        v.cooling = (flags & PURSUIT_COOLDOWN) != 0;
        v.left = left10 / 10.f;
        return v;
    }
    inline uint16_t LeftToWire(float left) {
        if (!(left > 0.f)) return 0;
        float l = left * 10.f + 0.5f;
        return l >= (float)MAX_COOL_LEFT10 ? MAX_COOL_LEFT10 : (uint16_t)l;
    }

    enum class Cool {
        Free,    // our game runs its cooldown as usual
        Hold,    // the other player is still seen: ours must not start, and starts over (as if we were seen)
        Delay,   // both out of sight but they need longer: hold ours back to their time, so you escape together
    };
    struct Decision { Cool what; float left; };   // left: the time ours should show (Delay)
    inline Decision DecideCooldown(const View& me, const View& peer) {
        if (!me.inPursuit || !peer.inPursuit) return { Cool::Free, 0.f };   // only one of you is chased
        if (peer.seen) return { Cool::Hold, 0.f };
        if (me.seen) return { Cool::Free, 0.f };                             // our own cops see us: the game resets it
        if (peer.left > me.left + DELAY_SLACK_S) return { Cool::Delay, peer.left };
        return { Cool::Free, 0.f };
    }

    // The game's escape progress is accum + sinceSeen (AIPursuit +0x124 / +0x128 from IPursuit*) and the
    // cooldown left is required - progress. Lower the progress so that `wantLeft` is left, taking it from
    // accum first; sinceSeen never goes down to the "seen" limit (that would end the cooldown).
    inline void DelayProgress(float required, float wantLeft, float& accum, float& sinceSeen) {
        if (!(required > 0.f) || !(wantLeft >= 0.f) || !(accum >= 0.f) || !(sinceSeen >= 0.f)) return;
        float want = required - wantLeft;
        if (want < 0.f) want = 0.f;
        float excess = accum + sinceSeen - want;
        if (excess <= 0.f) return;
        float a = excess < accum ? excess : accum;
        accum -= a; excess -= a;
        float room = sinceSeen - (SEEN_LIMIT_S + 0.1f);
        if (excess > 0.f && room > 0.f) sinceSeen -= excess < room ? excess : room;
    }

    // ---- busted ------------------------------------------------------------------------------------
    // CarState::bustSeq counts the sender's own busts. True once for every change after the first value
    // we saw (the first one only tells us where they are).
    struct BustTracker { bool have = false; uint8_t last = 0; };
    inline bool PeerJustBusted(BustTracker& t, uint8_t peerSeq) {
        if (!t.have) { t.have = true; t.last = peerSeq; return false; }
        if (peerSeq == t.last) return false;
        t.last = peerSeq;
        return true;
    }
}
