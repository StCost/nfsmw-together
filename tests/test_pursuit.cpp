// Shared pursuits (src/pursuitutil.h): heat sharing, cooldown hold / delay, busted together, and the
// pursuit fields of the state packet through the real handler.
#include "net_helpers.h"
#include "../src/pursuitutil.h"
#include <limits>

using namespace SharedPursuit;

// ---- heat ----------------------------------------------------------------------------------------
TEST(pursuit_heat_takes_the_hotter_one) {
    HeatTracker t;
    CHECK_NEAR(HeatToTake(t, 1.0f, 250), 2.5f, 1e-4);   // first value from them, hotter: take it
    HeatTracker u;
    CHECK_EQ(HeatToTake(u, 3.0f, 250), 0);               // ours is hotter: keep ours (they take ours)
}

TEST(pursuit_heat_only_rises_are_copied) {
    HeatTracker t;
    CHECK_EQ(HeatToTake(t, 3.0f, 300), 0);               // same heat: nothing to do
    CHECK_EQ(HeatToTake(t, 3.0f, 300), 0);
    CHECK_NEAR(HeatToTake(t, 3.0f, 350), 3.5f, 1e-4);   // they got heat: so do we
    // they were busted / changed car: their heat drops - ours stays
    CHECK_EQ(HeatToTake(t, 3.5f, 100), 0);
    // and their old value never comes back to push ours: a later rise below ours changes nothing
    CHECK_EQ(HeatToTake(t, 3.5f, 150), 0);
    // a rise above ours is copied again
    CHECK_NEAR(HeatToTake(t, 3.5f, 400), 4.0f, 1e-4);
}

TEST(pursuit_heat_dropped_on_our_side_is_not_pushed_back) {
    // we had 4, got busted (heat 1). Their 4 is not new, so it is not copied back to us.
    HeatTracker t;
    CHECK_EQ(HeatToTake(t, 4.0f, 400), 0);
    CHECK_EQ(HeatToTake(t, 1.0f, 400), 0);
    CHECK_EQ(HeatToTake(t, 1.0f, 400), 0);
}

TEST(pursuit_heat_gap_starts_over) {
    HeatTracker t;
    CHECK_EQ(HeatToTake(t, 2.0f, 200), 0);
    CHECK_EQ(HeatToTake(t, 2.0f, 0), 0);                 // they went into a race / menu: unknown
    CHECK_EQ(t.lastPeer, 0);
    CHECK_NEAR(HeatToTake(t, 1.0f, 200), 2.0f, 1e-4);   // back in free roam: the hotter one again
}

TEST(pursuit_heat_rounding_never_ratchets) {
    // both sides rise slowly; copying each other's rounded value must not run away
    HeatTracker a, b;
    float ha = 2.34f, hb = 2.31f;
    for (int i = 0; i < 200; ++i) {
        float ta = HeatToTake(a, ha, HeatToWire(hb)); if (ta > 0) ha = ta;
        float tb = HeatToTake(b, hb, HeatToWire(ha)); if (tb > 0) hb = tb;
    }
    CHECK(ha <= 2.35f && hb <= 2.35f);
    CHECK_NEAR(ha, hb, 0.02);
}

TEST(pursuit_heat_bad_values) {
    HeatTracker t;
    CHECK_EQ(HeatToTake(t, 1.0f, MAX_HEAT100 + 1), 0);   // out of range: ignored
    CHECK_EQ(t.lastPeer, 0);
    float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_EQ(HeatToTake(t, nan, 300), 0);                // our own heat unreadable: leave it
    CHECK_EQ(HeatToWire(0.f), 0);
    CHECK_EQ(HeatToWire(-3.f), 0);
    CHECK_EQ(HeatToWire(nan), 0);
    CHECK_EQ(HeatToWire(2.345f), 235);
    CHECK_EQ(HeatToWire(1.0e9f), MAX_HEAT100);
}

// ---- cooldown ------------------------------------------------------------------------------------
namespace {
    View V(bool in, bool seen, bool cooling, float left) { return View{ in, seen, cooling, left }; }
}

TEST(pursuit_cooldown_waits_while_the_other_is_seen) {
    // we are out of sight, they are still seen: ours must not start
    Decision d = DecideCooldown(V(true, false, false, 20), V(true, true, false, 20));
    CHECK(d.what == Cool::Hold);
    // ours already runs and they get spotted again: ours starts over
    d = DecideCooldown(V(true, false, true, 5), V(true, true, false, 20));
    CHECK(d.what == Cool::Hold);
    // both seen: the game resets ours anyway, holding changes nothing
    d = DecideCooldown(V(true, true, false, 20), V(true, true, false, 20));
    CHECK(d.what == Cool::Hold);
}

TEST(pursuit_cooldown_runs_when_both_out_of_sight) {
    Decision d = DecideCooldown(V(true, false, true, 10), V(true, false, true, 10));
    CHECK(d.what == Cool::Free);
    d = DecideCooldown(V(true, false, true, 10), V(true, false, true, 10 + DELAY_SLACK_S));   // within the slack
    CHECK(d.what == Cool::Free);
    d = DecideCooldown(V(true, false, true, 4), V(true, false, true, 9));                     // they lag: we wait
    CHECK(d.what == Cool::Delay);
    CHECK_NEAR(d.left, 9, 1e-4);
    d = DecideCooldown(V(true, false, true, 9), V(true, false, true, 4));                     // we lag: they wait
    CHECK(d.what == Cool::Free);
}

TEST(pursuit_cooldown_alone_is_untouched) {
    CHECK(DecideCooldown(V(true, false, true, 3), V(false, false, false, 0)).what == Cool::Free);   // they aren't chased
    CHECK(DecideCooldown(V(false, false, false, 0), V(true, true, false, 9)).what == Cool::Free);   // we aren't chased
    CHECK(DecideCooldown(V(true, true, false, 20), V(true, false, true, 3)).what == Cool::Free);   // we are seen ourselves
}

TEST(pursuit_cooldown_no_deadlock_from_the_hold) {
    // each side sends whether ITS OWN cops see it (not the held state). Walk a whole escape:
    View a = V(true, true, false, 20), b = V(true, true, false, 20);
    CHECK(DecideCooldown(a, b).what == Cool::Hold);
    a.seen = false;                                             // A gets away; B is still seen
    CHECK(DecideCooldown(a, b).what == Cool::Hold);             // A waits
    CHECK(DecideCooldown(b, a).what == Cool::Free);             // B: its own cops see it, nothing to do
    b.seen = false;                                             // B gets away too
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
}

TEST(pursuit_delay_progress) {
    // needed 30 s, progress 20 (search 5 + since seen 15) -> 10 left; the other one has 14 left
    float search = 5, since = 15;
    DelayProgress(30, 14, search, since);
    CHECK_NEAR(search, 1, 1e-4);                         // taken from the search time first
    CHECK_NEAR(since, 15, 1e-4);
    search = 1; since = 15;
    DelayProgress(30, 20, search, since);                // 10 more: search to 0, then since seen
    CHECK_NEAR(search, 0, 1e-4);
    CHECK_NEAR(since, 10, 1e-4);
    // never back down to the "seen" limit: that would end the cooldown
    search = 0; since = 9;
    DelayProgress(30, 29, search, since);
    CHECK_NEAR(since, SEEN_LIMIT_S + 0.1f, 1e-4);
    // already behind: nothing changes
    search = 2; since = 8;
    DelayProgress(30, 5, search, since);
    CHECK_NEAR(search, 2, 1e-4); CHECK_NEAR(since, 8, 1e-4);
    // garbage in: untouched
    float nan = std::numeric_limits<float>::quiet_NaN();
    search = 2; since = 8;
    DelayProgress(nan, 5, search, since);
    DelayProgress(30, nan, search, since);
    CHECK_NEAR(search, 2, 1e-4); CHECK_NEAR(since, 8, 1e-4);
}

TEST(pursuit_wire_view) {
    View v = FromWire(PURSUIT_ACTIVE | PURSUIT_COOLDOWN, 123);
    CHECK(v.inPursuit); CHECK(!v.seen); CHECK(v.cooling);
    CHECK_NEAR(v.left, 12.3, 1e-4);
    v = FromWire(PURSUIT_ACTIVE | PURSUIT_SEEN, 0);
    CHECK(v.seen); CHECK(!v.cooling);
    CHECK_EQ(LeftToWire(12.34f), 123);
    CHECK_EQ(LeftToWire(-1.f), 0);
    CHECK_EQ(LeftToWire(1.0e9f), MAX_COOL_LEFT10);
}

// ---- busted --------------------------------------------------------------------------------------
TEST(pursuit_busted_follows_changes_only) {
    BustTracker t;
    CHECK(!PeerJustBusted(t, 7));      // first value: only where they are (no bust on connect)
    CHECK(!PeerJustBusted(t, 7));
    CHECK(PeerJustBusted(t, 8));       // they were busted
    CHECK(!PeerJustBusted(t, 8));      // once
    CHECK(PeerJustBusted(t, 9));
    CHECK(!PeerJustBusted(t, 9));
    BustTracker w;                     // the counter wraps
    CHECK(!PeerJustBusted(w, 255));
    CHECK(PeerJustBusted(w, 0));
}

// ---- the state packet ---------------------------------------------------------------------------
TEST(pursuit_state_fields_round_trip) {
    th::NetFixture f(true);
    th::Connect();
    StatePacket p = th::MakeState(5, 1.0f, 10, 0);
    p.s.heat100 = 345; p.s.pursuitFlags = PURSUIT_ACTIVE | PURSUIT_SEEN; p.s.bustSeq = 3; p.s.coolLeft10 = 150;
    th::Inject(p);
    const Net::Snapshot& l = Net::Latest();
    REQUIRE(l.valid);
    CHECK_EQ(l.s.heat100, 345);
    CHECK_EQ(l.s.pursuitFlags, PURSUIT_ACTIVE | PURSUIT_SEEN);
    CHECK_EQ(l.s.bustSeq, 3);
    CHECK_EQ(l.s.coolLeft10, 150);
}

TEST(pursuit_state_bad_fields_are_dropped) {
    th::NetFixture f(true);
    th::Connect();
    StatePacket good = th::MakeState(5, 1.0f, 10, 0);
    good.s.heat100 = 200;
    th::Inject(good);
    StatePacket bad = th::MakeState(6, 1.1f, 11, 0);
    bad.s.heat100 = 60000;                                // absurd heat: the whole state is refused
    th::Inject(bad);
    bad = th::MakeState(7, 1.2f, 12, 0);
    bad.s.pursuitFlags = 0xF0;
    th::Inject(bad);
    CHECK_EQ(Net::Latest().seq, 5);
    CHECK_EQ(Net::Latest().s.heat100, 200);
}
