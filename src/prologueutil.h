#pragma once
// Co-op in the prologue (tests\test_prologue.cpp). The prologue (career bin 16) is a fixed chain of
// scripted events (16.1.0, 16.1.1, 16.2.2, 16.2.3, 16.8.1, 16.2.1) that each game loads by itself, one
// after the other. In co-op both games run their own chain; they are kept together at each start: the
// player who is ahead waits on the grid until the other player is on the same step.
// The step and the "I'm in the prologue" flag travel in CarState::pad2 (older builds send 0 there).
#include <cstdint>

namespace Prologue {
    constexpr uint16_t Supported = 0x8000;   // the sender's build knows co-op in the prologue
    constexpr uint16_t InPrologue = 0x4000;  // the sender's career is in the prologue
    constexpr uint16_t StepMask = 0x00FF;    // prologue events the sender has done

    inline uint16_t Encode(bool inPrologue, int step) {
        if (step < 0) step = 0; if (step > 255) step = 255;
        return (uint16_t)(Supported | (inPrologue ? InPrologue : 0) | step);
    }
    struct Peer { bool known; bool supported; bool inPrologue; int step; };
    inline Peer Decode(bool valid, uint16_t pad2) {
        Peer p{};
        p.known = valid;
        p.supported = valid && (pad2 & Supported);
        p.inPrologue = p.supported && (pad2 & InPrologue);
        p.step = p.supported ? (pad2 & StepMask) : 0;
        return p;
    }
    // CarState::pad[1] ("co-op allowed here"). In the prologue only towards a peer that knows prologue co-op
    // (an older build would take us for a career past the prologue).
    inline bool AllowedHere(bool careerStarted, bool inPrologue, bool optionOn, const Peer& peer) {
        if (!careerStarted) return false;
        if (!inPrologue) return true;
        return optionOn && peer.supported;
    }
    // Linked = both allowed and both in the same part of the career (both in the prologue, or both past it).
    // A peer we have no state from yet: as before past the prologue (allowed), not yet in it.
    inline bool PhaseMatches(bool inPrologue, const Peer& peer) {
        if (!peer.known) return !inPrologue;
        return peer.inPrologue == inPrologue;
    }
    // The prologue events: "16.<x>.<y>".
    inline bool IsPrologueEvent(const char* id) { return id && id[0] == '1' && id[1] == '6' && id[2] == '.'; }

    // A rival (boss) event "<bin>.x.y" of the other player is only joined by a career at that rival (its
    // Blacklist number = the event's bin); other events from anywhere, as before. Unknown bin: joined.
    inline bool BossEventJoinable(bool boss, const char* id, int careerBin) {
        if (!boss || !id || careerBin <= 0) return true;
        int bin = 0;
        for (const char* c = id; *c >= '0' && *c <= '9'; ++c) bin = bin * 10 + (*c - '0');
        return bin == 0 || bin == careerBin;
    }
    // On the grid of a prologue event: wait for the other player while they are still on the way here -
    // not past this step (their step <= ours) and not on this grid yet. A player ahead of us waits for us
    // instead; we don't wait for them.
    inline bool WaitForPeer(bool inPrologue, int myStep, const Peer& peer, bool peerOnOurGrid) {
        if (!inPrologue || !peer.inPrologue || peerOnOurGrid) return false;
        return peer.step <= myStep;
    }
    // Shared pursuits / following in the prologue: only on the same step (a pursuit there must not pull a
    // player who is in another scripted part of the chain).
    inline bool SameStep(bool inPrologue, int myStep, const Peer& peer) {
        if (!inPrologue) return !peer.inPrologue;
        return peer.inPrologue && peer.step == myStep;
    }
}
