// Pure helpers from src/netutil.h: names, traffic ids, race traffic binding, interpolation math, validation.
#include "net_helpers.h"
#include <limits>

using namespace NetUtil;

TEST(util_hud_name_truncates_to_15) {
    char buf[32];
    memset(buf, 'x', sizeof(buf));
    CopyHudName(buf, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    CHECK_STR(buf, "ABCDEFGHIJKLMNO");               // 15 characters
    CHECK_EQ(strlen(buf), 15);
    CHECK_EQ(buf[16], 'x');                          // nothing written past the 16-byte slot

    CopyHudName(buf, "Short");
    CHECK_STR(buf, "Short");
    CopyHudName(buf, "123456789012345");             // exactly 15 fits
    CHECK_STR(buf, "123456789012345");
    CopyHudName(buf, "");
    CHECK_STR(buf, "");
    CopyHudName(buf, nullptr);
    CHECK_STR(buf, "");
}

TEST(util_traffic_id_scheme) {
    CHECK_EQ(CLIENT_ID_BIT, 0x8000);
    // host ids go out as they are, client ids carry bit 15
    CHECK_EQ(TrafficWireId(5, true), 5);
    CHECK_EQ(TrafficWireId(5, false), 0x8005);
    CHECK(!TrafficIdIsClients(5));
    CHECK(TrafficIdIsClients(0x8005));
    // each side mirrors only the other side's cars
    CHECK(TrafficIdAccepted(0x8005, /*weAreHost*/ true));
    CHECK(!TrafficIdAccepted(5, true));
    CHECK(TrafficIdAccepted(5, false));
    CHECK(!TrafficIdAccepted(0x8005, false));
    // a car never comes back to its owner as a mirror
    for (uint16_t id : { (uint16_t)1, (uint16_t)77, (uint16_t)0x7FFF }) {
        CHECK(!TrafficIdAccepted(TrafficWireId(id, true), true));
        CHECK(!TrafficIdAccepted(TrafficWireId(id, false), false));
        CHECK(TrafficIdAccepted(TrafficWireId(id, true), false));
        CHECK(TrafficIdAccepted(TrafficWireId(id, false), true));
    }
}

TEST(util_traffic_id_allocation_wraps) {
    CHECK_EQ(NextTrafficLocalId(1), 2);
    CHECK_EQ(NextTrafficLocalId(0x7FFE), 0x7FFF);
    CHECK_EQ(NextTrafficLocalId(0x7FFF), 1);         // wraps before the client bit, skipping 0
    CHECK_EQ(NextTrafficLocalId(0xFFFF), 1);
    uint16_t id = 1;
    for (int i = 0; i < 70000; ++i) {
        id = NextTrafficLocalId(id);
        if (id == 0 || (id & CLIENT_ID_BIT)) { CHECK(!"local id left 1..0x7FFF"); break; }
    }
}

TEST(util_race_traffic_partner_prefers_same_model) {
    const float at[3] = { 0, 0, 0 };
    TrafficCandidate c[4] = {
        { 0xA, {   5, 0, 0 }, false },   // other model, nearest
        { 0xB, { 300, 0, 0 }, false },   // same model, far
        { 0xB, { 100, 0, 0 }, false },   // same model, nearer
        { 0xB, {   1, 0, 0 }, true  },   // same model, nearest, but taken
    };
    CHECK_EQ(PickTrafficPartner(0xB, at, c, 4, false), 2);
    CHECK_EQ(PickTrafficPartner(0xB, at, c, 4, true), 2);     // same model wins over a nearer other model
    CHECK_EQ(PickTrafficPartner(0xC, at, c, 4, false), -1);   // no such model, any model not allowed
    CHECK_EQ(PickTrafficPartner(0xC, at, c, 4, true), 0);     // nearest of any model
}

TEST(util_race_traffic_partner_edge_cases) {
    const float at[3] = { 10, 0, 10 };
    CHECK_EQ(PickTrafficPartner(1, at, nullptr, 0, true), -1);
    TrafficCandidate all[2] = { { 1, { 0, 0, 0 }, true }, { 2, { 0, 0, 0 }, true } };
    CHECK_EQ(PickTrafficPartner(1, at, all, 2, true), -1);    // everything taken
    const float nan = std::numeric_limits<float>::quiet_NaN();
    TrafficCandidate bad[2] = { { 1, { nan, 0, 0 }, false }, { 1, { 50, 0, 50 }, false } };
    CHECK_EQ(PickTrafficPartner(1, at, bad, 2, true), 1);     // a NaN position is never picked
    // greedy binding with `skip` never gives one car to two host cars
    TrafficCandidate c[3] = { { 7, { 0, 0, 0 }, false }, { 7, { 20, 0, 0 }, false }, { 8, { 40, 0, 0 }, false } };
    bool used[3] = {};
    for (int host = 0; host < 4; ++host) {
        int k = PickTrafficPartner(7, at, c, 3, true);
        if (host < 3) { REQUIRE(k >= 0); CHECK(!used[k]); used[k] = true; c[k].skip = true; }
        else CHECK_EQ(k, -1);
    }
    CHECK(used[2]);                                           // the third host car got the other model
}

TEST(util_hermite) {
    // endpoints
    CHECK_NEAR(Hermite(3, 7, 9, -2, 0.5f, 0.0f), 3, 1e-5);
    CHECK_NEAR(Hermite(3, 7, 9, -2, 0.5f, 1.0f), 9, 1e-5);
    // constant velocity is reproduced exactly: p(t) = 10 + 20 t over 0.1 s
    for (float u = 0; u <= 1.0f; u += 0.125f)
        CHECK_NEAR(Hermite(10, 20, 12, 20, 0.1f, u), 10 + 2 * u, 1e-4);
    // constant acceleration (v 0 -> 10 over 1 s, 5 m travelled): midpoint is 1.25 m
    CHECK_NEAR(Hermite(0, 0, 5, 10, 1.0f, 0.5f), 1.25, 1e-4);
    // standing still stays still
    CHECK_NEAR(Hermite(4, 0, 4, 0, 0.1f, 0.3f), 4, 1e-5);
    // zero time span: tangents drop out, no NaN
    float z = Hermite(1, 100, 2, 100, 0.0f, 0.5f);
    CHECK_NEAR(z, 1.5, 1e-5);
}

TEST(util_lerp3) {
    float a[3] = { 0, 10, -4 }, b[3] = { 2, 20, 4 }, o[3];
    Lerp3(a, b, 0.25f, o);
    CHECK_NEAR(o[0], 0.5, 1e-6); CHECK_NEAR(o[1], 12.5, 1e-6); CHECK_NEAR(o[2], -2, 1e-6);
    Lerp3(a, b, 0, o); CHECK_NEAR(o[1], 10, 1e-6);
    Lerp3(a, b, 1, o); CHECK_NEAR(o[1], 20, 1e-6);
}

static float QLen(const float* q) { return sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); }

TEST(util_nlerp) {
    const float id[4] = { 0, 0, 0, 1 };
    const float s = 0.70710678f;
    const float z90[4] = { 0, 0, s, s };             // 90 degrees about z
    float o[4];

    NlerpQ(id, z90, 0, o);
    CHECK_NEAR(o[3], 1, 1e-5); CHECK_NEAR(o[2], 0, 1e-5);
    NlerpQ(id, z90, 1, o);
    CHECK_NEAR(o[2], s, 1e-5); CHECK_NEAR(o[3], s, 1e-5);

    // halfway is the 45 degree rotation, and the result is unit length
    NlerpQ(id, z90, 0.5f, o);
    CHECK_NEAR(QLen(o), 1, 1e-5);
    CHECK_NEAR(o[2], sinf(3.14159265f / 8), 1e-4);
    CHECK_NEAR(o[3], cosf(3.14159265f / 8), 1e-4);

    // q and -q are the same rotation: must take the short way, not swing through zero
    const float negZ90[4] = { 0, 0, -s, -s };
    float o2[4];
    NlerpQ(id, negZ90, 0.5f, o2);
    CHECK_NEAR(QLen(o2), 1, 1e-5);
    for (int i = 0; i < 4; ++i) CHECK_NEAR(o2[i], o[i], 1e-5);

    // always unit length for unit inputs
    for (float t = 0; t <= 1.0f; t += 0.1f) { NlerpQ(id, z90, t, o); CHECK_NEAR(QLen(o), 1, 1e-5); }

    // degenerate input (all zeros, e.g. a peer that isn't driving): no NaN, no division by zero
    const float zero[4] = { 0, 0, 0, 0 };
    NlerpQ(zero, zero, 0.5f, o);
    for (int i = 0; i < 4; ++i) CHECK(o[i] == 0.0f);
}

TEST(util_validation_rejects_bad_floats) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    CarState s{};                                    // all zeros: "not driving" - valid
    CHECK(ValidCarState(s));
    s = th::MakeState(1, 12.5f, 3000.0f, 90.0f).s;
    CHECK(ValidCarState(s));

    CarState b = s; b.pos[1] = nan;        CHECK(!ValidCarState(b));
    b = s; b.pos[2] = inf;                 CHECK(!ValidCarState(b));
    b = s; b.pos[0] = -inf;                CHECK(!ValidCarState(b));
    b = s; b.pos[0] = 5.0e6f;              CHECK(!ValidCarState(b));   // outside any game world
    b = s; b.rot[0] = nan;                 CHECK(!ValidCarState(b));
    b = s; b.rot[3] = 50.0f;               CHECK(!ValidCarState(b));   // not a rotation
    b = s; b.linVel[2] = 1.0e30f;          CHECK(!ValidCarState(b));
    b = s; b.angVel[0] = nan;              CHECK(!ValidCarState(b));
    b = s; b.speedMps = inf;               CHECK(!ValidCarState(b));
    b = s; b.simTime = nan;                CHECK(!ValidCarState(b));
    b = s; b.simTime = inf;                CHECK(!ValidCarState(b));
    b = s; b.steer = nan;                  CHECK(!ValidCarState(b));
    b = s; b.steer = 40.0f;                CHECK(!ValidCarState(b));   // not a steering value
    b = s; b.rot[0] = b.rot[1] = b.rot[2] = b.rot[3] = 0; CHECK(!ValidCarState(b));   // a car with no rotation
    b = s; b.racePct = nan;                CHECK(!ValidCarState(b));
    b = s; b.racePct = 1.0e30f;            CHECK(!ValidCarState(b));
    b = s; b.lapPct = -50.0f;              CHECK(!ValidCarState(b));
    b = s; b.laps = -3;                    CHECK(!ValidCarState(b));
    b = s; b.checkpoints = 30000;          CHECK(!ValidCarState(b));
    // shared pursuit fields
    b = s; b.heat100 = MAX_HEAT100; b.coolLeft10 = MAX_COOL_LEFT10; b.pursuitFlags = PURSUIT_KNOWN_BITS; b.bustSeq = 255;
    CHECK(ValidCarState(b));
    b = s; b.heat100 = MAX_HEAT100 + 1;    CHECK(!ValidCarState(b));
    b = s; b.heat100 = 0xFFFF;             CHECK(!ValidCarState(b));
    b = s; b.coolLeft10 = MAX_COOL_LEFT10 + 1; CHECK(!ValidCarState(b));
    b = s; b.pursuitFlags = 0x10;          CHECK(!ValidCarState(b));   // a bit this build doesn't know
    b = s; b.pursuitFlags = 0x80;          CHECK(!ValidCarState(b));
    b = s; b.racePct = 100.0f; b.laps = 3; CHECK(ValidCarState(b));    // a finished race

    TrafficEntry e = th::MakeTrafficEntry(1, 100.0f);
    CHECK(ValidTrafficEntry(e));
    TrafficEntry eb = e; eb.pos[0] = nan;  CHECK(!ValidTrafficEntry(eb));
    eb = e; eb.linVel[1] = inf;            CHECK(!ValidTrafficEntry(eb));
    eb = e; eb.rot[2] = nan;               CHECK(!ValidTrafficEntry(eb));
    eb = e; eb.angVel[2] = -1.0e20f;       CHECK(!ValidTrafficEntry(eb));

    // limits are inclusive, signs don't matter
    float v[3] = { -MAX_POS, MAX_POS, 0 };
    CHECK(FiniteWithin(v, 3, MAX_POS));
    v[0] = -MAX_POS * 1.01f;
    CHECK(!FiniteWithin(v, 3, MAX_POS));
}

// ---- race standings: the other player's % moved on to "now" ----
TEST(util_race_pct_extrapolates_at_recent_rate) {
    // 10 % -> 11 % in 200 ms = 0.005 %/ms; 40 ms past the newest sample: + 0.2 %
    CHECK_NEAR(RacePctAt(10.f, 1000.0, 11.f, 1200.0, 1240.0), 11.2, 1e-4);
    // not past the newest sample (or exactly on it): the newest value as it is
    CHECK_NEAR(RacePctAt(10.f, 1000.0, 11.f, 1200.0, 1200.0), 11.0, 1e-5);
    CHECK_NEAR(RacePctAt(10.f, 1000.0, 11.f, 1200.0, 1100.0), 11.0, 1e-5);
    // late packets: at most 250 ms ahead (or what the caller allows)
    CHECK_NEAR(RacePctAt(10.f, 1000.0, 11.f, 1200.0, 5000.0), 11.0 + 0.005 * 250, 1e-4);
    CHECK_NEAR(RacePctAt(10.f, 1000.0, 11.f, 1200.0, 1400.0, 100.0), 11.5, 1e-4);
}

TEST(util_race_pct_never_backwards_or_past_100) {
    CHECK_NEAR(RacePctAt(50.f, 1000.0, 0.f, 1100.0, 1150.0), 0.0, 1e-6);       // restart: % dropped, no negative rate
    CHECK_NEAR(RacePctAt(5.f, 1000.0, 5.f, 1100.0, 1150.0), 5.0, 1e-6);        // standing still
    CHECK_NEAR(RacePctAt(99.9f, 1000.0, 99.99f, 1020.0, 1200.0), 100.0, 1e-6); // capped at the finish
    CHECK_NEAR(RacePctAt(1.f, 1000.0, 90.f, 1100.0, 1150.0), 90.0, 1e-6);      // a jump, not a speed: not extrapolated
    CHECK_NEAR(RacePctAt(1.f, 1000.0, 2.f, 1010.0, 1100.0), 2.0, 1e-6);        // samples too close together
    CHECK_NEAR(RacePctAt(3.f, 1000.0, 3.f, 1000.0, 1100.0), 3.0, 1e-6);        // a single sample
    float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_NEAR(RacePctAt(nan, 1000.0, 4.f, 1100.0, 1150.0), 4.0, 1e-6);
    CHECK_NEAR(RacePctAt(1.f, 1000.0, nan, 1100.0, 1150.0), 0.0, 1e-6);
    CHECK_NEAR(RacePctAt(-5.f, 1000.0, -1.f, 1100.0, 1100.0), 0.0, 1e-6);
}

TEST(util_race_order_switches_within_metres) {
    // A 6 km race: 1 m = 1/60 %. Both drive 50 m/s; the other player is 2 m ahead of us right now, but
    // their newest sample is 80 ms old (half the ping + time since it was sent). Moved on to now, their
    // % is ahead of ours, as it should be.
    const double perM = 100.0 / 6000.0;
    double ourM = 3000.0, theirNowM = 3002.0;
    double theirAtNewest = theirNowM - 50.0 * 0.080, theirAtOlder = theirAtNewest - 50.0 * 0.150;
    float est = RacePctAt((float)(theirAtOlder * perM), 1000.0, (float)(theirAtNewest * perM), 1150.0, 1230.0);
    CHECK(est > (float)(ourM * perM));
    CHECK_NEAR(est / perM, theirNowM, 0.5);
    // without moving it on they would still count 2 m behind us
    CHECK((float)(theirAtNewest * perM) < (float)(ourM * perM));
}

// ---- race start: the client leaves the grid slot both games give their own player ----
TEST(util_take_peer_slot_only_when_sharing_a_slot) {
    float me[3] = { 100, 5, 200 }, peerSame[3] = { 100.5f, 5, 200.4f }, slot[3] = { 104, 5, 196 };
    CHECK(ShouldTakePeerSlot(me, 0.f, peerSame, 0.f, slot));
    CHECK(ShouldTakePeerSlot(me, 0.5f, peerSame, 0.9f, slot));                 // settling on the suspension
    float peerApart[3] = { 110, 5, 200 };
    CHECK(!ShouldTakePeerSlot(me, 0.f, peerApart, 0.f, slot));                 // already on different spots
    CHECK(!ShouldTakePeerSlot(me, 12.f, peerSame, 0.f, slot));                 // racing: never move a driving car
    CHECK(!ShouldTakePeerSlot(me, 0.f, peerSame, 12.f, slot));
    float slotHere[3] = { 100.5f, 5, 200 }, slotFar[3] = { 400, 5, 200 };
    CHECK(!ShouldTakePeerSlot(me, 0.f, peerSame, 0.f, slotHere));              // that slot is ours
    CHECK(!ShouldTakePeerSlot(me, 0.f, peerSame, 0.f, slotFar));               // not a grid slot next to us
    float nan = std::numeric_limits<float>::quiet_NaN();
    float bad[3] = { nan, 5, 200 };
    CHECK(!ShouldTakePeerSlot(me, 0.f, bad, 0.f, slot));
    CHECK(!ShouldTakePeerSlot(me, nan, peerSame, 0.f, slot));
    CHECK(!ShouldTakePeerSlot(me, 0.f, peerSame, 0.f, bad));
}
