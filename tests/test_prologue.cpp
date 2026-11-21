// Co-op in the prologue (src/prologueutil.h).
#include "mwt_test.h"
#include "../src/prologueutil.h"

using namespace Prologue;

TEST(prologue_encode_decode) {
    Peer p = Decode(true, Encode(true, 3));
    CHECK(p.known); CHECK(p.supported); CHECK(p.inPrologue); CHECK_EQ(p.step, 3);
    p = Decode(true, Encode(false, 6));
    CHECK(p.supported); CHECK(!p.inPrologue); CHECK_EQ(p.step, 6);
    p = Decode(true, 0);                                   // an older build
    CHECK(p.known); CHECK(!p.supported); CHECK(!p.inPrologue);
    p = Decode(false, Encode(true, 2));                    // no state yet
    CHECK(!p.known); CHECK(!p.supported);
    CHECK_EQ(Decode(true, Encode(true, 999)).step, 255);
}

TEST(prologue_allowed_here) {
    Peer newPeer = Decode(true, Encode(true, 0)), oldPeer = Decode(true, 0);
    CHECK(!AllowedHere(false, false, true, newPeer));      // career not started
    CHECK(AllowedHere(true, false, true, oldPeer));        // past the prologue: as before
    CHECK(AllowedHere(true, true, true, newPeer));
    CHECK(!AllowedHere(true, true, true, oldPeer));        // an older build would take us for past the prologue
    CHECK(!AllowedHere(true, true, false, newPeer));       // [Coop] Prologue=0
}

TEST(prologue_phase) {
    CHECK(PhaseMatches(true, Decode(true, Encode(true, 1))));
    CHECK(PhaseMatches(false, Decode(true, Encode(false, 6))));
    CHECK(!PhaseMatches(true, Decode(true, Encode(false, 6))));   // one in the prologue, one past it
    CHECK(!PhaseMatches(false, Decode(true, Encode(true, 1))));
    CHECK(PhaseMatches(false, Decode(true, 0)));                  // an older build past the prologue
    CHECK(!PhaseMatches(true, Decode(true, 0)));
    CHECK(PhaseMatches(false, Decode(false, 0)));                 // no state yet: as before
    CHECK(!PhaseMatches(true, Decode(false, 0)));
}

TEST(prologue_event_ids) {
    CHECK(IsPrologueEvent("16.2.1")); CHECK(IsPrologueEvent("16.1.0"));
    CHECK(!IsPrologueEvent("15.1.1")); CHECK(!IsPrologueEvent("1.6.1")); CHECK(!IsPrologueEvent(nullptr)); CHECK(!IsPrologueEvent("16"));
}

TEST(prologue_wait_on_grid) {
    Peer behind = Decode(true, Encode(true, 1)), same = Decode(true, Encode(true, 2)), ahead = Decode(true, Encode(true, 3));
    CHECK(WaitForPeer(true, 2, behind, false));            // still racing the step before
    CHECK(WaitForPeer(true, 2, same, false));              // between events, on the way here
    CHECK(!WaitForPeer(true, 2, same, true));              // here on our grid
    CHECK(!WaitForPeer(true, 2, ahead, false));            // ahead: they wait for us
    CHECK(!WaitForPeer(true, 2, Decode(true, Encode(false, 6)), false));   // past the prologue
    CHECK(!WaitForPeer(false, 2, behind, false));
    CHECK(!WaitForPeer(true, 2, Decode(true, 0), false));  // older build
}

TEST(prologue_same_step) {
    CHECK(SameStep(true, 2, Decode(true, Encode(true, 2))));
    CHECK(!SameStep(true, 2, Decode(true, Encode(true, 1))));
    CHECK(!SameStep(true, 2, Decode(true, Encode(false, 6))));
    CHECK(SameStep(false, 6, Decode(true, Encode(false, 6))));
    CHECK(SameStep(false, 6, Decode(true, 0)));
    CHECK(!SameStep(false, 6, Decode(true, Encode(true, 2))));
}

TEST(boss_event_join_needs_the_same_rival) {
    CHECK(BossEventJoinable(true, "1.8.1", 1));             // Razor's final pursuit at rival #1
    CHECK(!BossEventJoinable(true, "1.8.1", 3));            // ... not for a career at rival #3
    CHECK(!BossEventJoinable(true, "15.1.2", 14));
    CHECK(BossEventJoinable(false, "1.2.1", 9));            // a normal race of another bin: as before
    CHECK(BossEventJoinable(true, "15.1.2", 0));            // our bin unknown: as before
    CHECK(BossEventJoinable(true, nullptr, 3));
}
