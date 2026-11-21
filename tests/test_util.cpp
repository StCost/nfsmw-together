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

// ---- smooth traffic handover ----
static CamView Cam(float x, float y, float z, float dx, float dy, float dz, bool valid = true) {
    CamView c{ { x, y, z }, { dx, dy, dz }, valid };
    return c;
}

TEST(util_handover_camera_cone) {
    CamView c = Cam(0, 0, 0, 0, 0, 1);                       // looking along +z (y is up)
    const float ahead[3] = { 0, 0, 50 }, behind[3] = { 0, 0, -50 }, side[3] = { 50, 0, 0 };
    const float ahead45[3] = { 40, 0, 40 }, back80[3] = { 50, 0, -9 }, farAhead[3] = { 0, 0, 400 };
    CHECK(InCameraView(c, ahead, HANDOVER_FAR_M));
    CHECK(InCameraView(c, ahead45, HANDOVER_FAR_M));          // inside the 70 degree half-cone
    CHECK(!InCameraView(c, behind, HANDOVER_FAR_M));
    CHECK(!InCameraView(c, side, HANDOVER_FAR_M));            // 90 degrees off: out of view
    CHECK(!InCameraView(c, back80, HANDOVER_FAR_M));
    CHECK(!InCameraView(c, farAhead, HANDOVER_FAR_M));        // straight ahead but beyond the far limit
    // a car at the cone's edge still shows half of itself: its size widens the cone
    const float edge[3] = { 10.f * sinf(71.f * 3.14159f / 180.f), 0, 10.f * cosf(71.f * 3.14159f / 180.f) };
    CHECK(InCameraView(c, edge, HANDOVER_FAR_M));
    const float edgeFar[3] = { 150.f * sinf(75.f * 3.14159f / 180.f), 0, 150.f * cosf(75.f * 3.14159f / 180.f) };
    CHECK(!InCameraView(c, edgeFar, HANDOVER_FAR_M));
    // a camera direction that isn't unit length works the same
    CamView c2 = Cam(0, 0, 0, 0, 0, 7);
    CHECK(InCameraView(c2, ahead, HANDOVER_FAR_M));
    CHECK(!InCameraView(c2, behind, HANDOVER_FAR_M));
    // right at the camera: seen
    const float here[3] = { 1, 0, -1 };
    CHECK(InCameraView(c, here, HANDOVER_FAR_M));
}

TEST(util_handover_camera_unknown_or_broken) {
    const float behind[3] = { 0, 0, -50 }, farBehind[3] = { 0, 0, -300 };
    CamView none = Cam(0, 0, 0, 0, 0, 1, false);              // unknown camera: everything near counts as seen
    CHECK(InCameraView(none, behind, HANDOVER_FAR_M));
    CHECK(!InCameraView(none, farBehind, HANDOVER_FAR_M));
    CamView zeroDir = Cam(0, 0, 0, 0, 0, 0);
    CHECK(InCameraView(zeroDir, behind, HANDOVER_FAR_M));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float bad[3] = { nan, 0, 0 };
    CHECK(InCameraView(Cam(0, 0, 0, 0, 0, 1), bad, HANDOVER_FAR_M));   // NaN position: assume seen
    CamView nanDir = Cam(0, 0, 0, nan, 0, 1);
    CHECK(InCameraView(nanDir, behind, HANDOVER_FAR_M));
}

TEST(util_handover_may_spawn_unseen) {
    CamView c = Cam(0, 2, -5, 0, 0, 1);                       // chase camera 5 m behind the player
    const float player[3] = { 0, 0, 0 };
    const float behind60[3] = { 0, 0, -60 }, behind20[3] = { 0, 0, -20 }, ahead60[3] = { 0, 0, 60 };
    const float ahead250[3] = { 0, 0, 250 }, side40[3] = { 40, 0, 0 };
    CHECK(MaySpawnUnseen(c, behind60, player));               // behind, not too close
    CHECK(!MaySpawnUnseen(c, behind20, player));              // behind but too close (look-back / mirror)
    CHECK(!MaySpawnUnseen(c, ahead60, player));               // right in front: wait
    CHECK(MaySpawnUnseen(c, ahead250, player));               // in front but far away
    CHECK(MaySpawnUnseen(c, side40, player));                 // beside the player, out of the cone
    CHECK(MaySpawnUnseen(c, behind60, nullptr));
    // unknown camera: only far from the player
    CamView none = Cam(0, 0, 0, 0, 0, 0, false);
    CHECK(!MaySpawnUnseen(none, behind60, player));
    CHECK(MaySpawnUnseen(none, ahead250, player));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float bad[3] = { nan, nan, nan };
    CHECK(!MaySpawnUnseen(c, bad, player));
}

TEST(util_handover_own_car_may_go) {
    CHECK(!OwnCarMayGo(0.5f, 0.f, 50.f));                     // on screen: never
    CHECK(!OwnCarMayGo(0.01f, 0.f, 300.f));
    CHECK(OwnCarMayGo(0.f, 2.f, 20.f));                       // off screen for a while
    CHECK(!OwnCarMayGo(0.f, 0.1f, 20.f));                     // just left the view: wait a moment
    CHECK(OwnCarMayGo(0.f, 0.f, 20.f));                       // not drawn at all
    CHECK(OwnCarMayGo(3.f, 0.f, 600.f));                      // far beyond drawing range
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(!OwnCarMayGo(nan, 5.f, 20.f));                      // garbage timer: keep
}

TEST(util_handover_takeover_partner) {
    const float at[3] = { 0, 0, 0 };
    TrafficCandidate c[4] = {
        { 0xA, {   5, 0, 0 }, false },   // other model, near
        { 0xB, {  25, 0, 0 }, false },   // same model, within the same-model range
        { 0xB, {  60, 0, 0 }, false },   // same model, too far to jump in view
        { 0xA, {  15, 0, 0 }, false },   // other model, beyond the any-model range
    };
    CHECK_EQ(PickTakeoverPartner(0xB, at, c, 4, 30.f, 10.f), 1);   // same model first
    CHECK_EQ(PickTakeoverPartner(0xC, at, c, 4, 30.f, 10.f), 0);   // else any model, only close
    c[0].skip = true;
    CHECK_EQ(PickTakeoverPartner(0xC, at, c, 4, 30.f, 10.f), -1);  // nothing close enough
    c[1].skip = true;
    CHECK_EQ(PickTakeoverPartner(0xB, at, c, 4, 30.f, 10.f), -1);  // the far one is never taken
    CHECK_EQ(PickTakeoverPartner(0xB, at, nullptr, 0, 30.f, 10.f), -1);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    TrafficCandidate bad[1] = { { 0xB, { nan, 0, 0 }, false } };
    CHECK_EQ(PickTakeoverPartner(0xB, at, bad, 1, 30.f, 10.f), -1);
}

// ---- no new cars in front of the other player ----
TEST(util_nospawn_in_front_of_the_other_player) {
    const float car[3] = { 100, 5, 200 };
    const float fwd[3] = { 0, 0, 1 };                          // facing +z (y is up)
    auto at = [&](float dx, float dz, float* o) { o[0] = car[0] + dx; o[1] = car[1]; o[2] = car[2] + dz; return o; };
    float p[3];
    CHECK(SpawnSpotBlocked(at(0, 40, p), car, fwd, 1));        // straight ahead, 40 m
    CHECK(SpawnSpotBlocked(at(20, 45, p), car, fwd, 1));       // ahead, a little to the side
    CHECK(!SpawnSpotBlocked(at(0, 70, p), car, fwd, 1));       // ahead but beyond 60 m
    CHECK(!SpawnSpotBlocked(at(0, -40, p), car, fwd, 1));      // behind
    CHECK(!SpawnSpotBlocked(at(45, -10, p), car, fwd, 1));     // beside and a bit behind: out of the cone
    CHECK(SpawnSpotBlocked(at(0, -10, p), car, fwd, 1));       // very close: any direction
    CHECK(SpawnSpotBlocked(at(12, 0, p), car, fwd, 1));
    // the cone starts at the chase camera behind the car: a spot close beside the car counts
    CHECK(SpawnSpotBlocked(at(18, 8, p), car, fwd, 1));
    // height counts in the distance (a bridge 70 m above), not in the cone
    const float above[3] = { car[0], car[1] + 70, car[2] + 10 };
    CHECK(!SpawnSpotBlocked(above, car, fwd, 1));
    const float slope[3] = { car[0], car[1] + 8, car[2] + 30 };
    CHECK(SpawnSpotBlocked(slope, car, fwd, 1));
    // a direction that isn't unit length (a velocity) works the same; two directions: either counts
    const float vel[3] = { 30, 0, 0 };                         // body points +z, sliding along +x
    const float two[6] = { 0, 0, 1, 30, 0, 0 };
    CHECK(SpawnSpotBlocked(at(40, 0, p), car, vel, 1));
    CHECK(SpawnSpotBlocked(at(40, 0, p), car, two, 2));
    CHECK(SpawnSpotBlocked(at(0, 40, p), car, two, 2));
    CHECK(!SpawnSpotBlocked(at(-40, -20, p), car, two, 2));
}

TEST(util_nospawn_unknown_heading_or_bad_input) {
    const float car[3] = { 0, 0, 0 };
    const float behind[3] = { 0, 0, -40 }, farAway[3] = { 0, 0, -80 };
    CHECK(SpawnSpotBlocked(behind, car, nullptr, 0));          // heading unknown: the whole circle
    CHECK(!SpawnSpotBlocked(farAway, car, nullptr, 0));
    const float zero[3] = { 0, 5, 0 };                         // straight up / zero: no usable direction
    CHECK(SpawnSpotBlocked(behind, car, zero, 1));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float nanDir[3] = { nan, 0, nan };
    CHECK(SpawnSpotBlocked(behind, car, nanDir, 1));
    const float bad[3] = { nan, 0, 0 };
    CHECK(!SpawnSpotBlocked(bad, car, nullptr, 0));            // a broken spot is not ours to refuse
    CHECK(!SpawnSpotBlocked(behind, bad, nullptr, 0));
}

TEST(util_nospawn_mirror_may_appear) {
    CamView c = Cam(0, 2, -5, 0, 0, 1);                       // chase camera 5 m behind the player
    const float ahead40[3] = { 0, 0, 40 }, ahead90[3] = { 0, 0, 90 }, behind30[3] = { 0, 0, -30 }, side50[3] = { 50, 0, 0 };
    CHECK(!MirrorMayAppear(c, ahead40));                      // in view and close: wait
    CHECK(MirrorMayAppear(c, ahead90));                       // in view but farther than 60 m
    CHECK(MirrorMayAppear(c, behind30));                      // behind the camera
    CHECK(MirrorMayAppear(c, side50));                        // out of the cone
    CamView none = Cam(0, 0, 0, 0, 0, 0, false);              // unknown camera: only farther than 60 m
    CHECK(!MirrorMayAppear(none, behind30));
    CHECK(MirrorMayAppear(none, ahead90));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float bad[3] = { nan, nan, nan };
    CHECK(!MirrorMayAppear(c, bad));
}

// ---- AI catch-up ----
TEST(util_catch_up_follows_the_leader) {
    float out = -1;
    CHECK(CatchUpLeaderPct(40.f, true, 55.f, out));  CHECK_EQ(out, 55.f);   // partner ahead
    CHECK(CatchUpLeaderPct(60.f, true, 55.f, out));  CHECK_EQ(out, 60.f);   // we are ahead
    CHECK(CatchUpLeaderPct(40.f, false, 90.f, out)); CHECK_EQ(out, 40.f);   // partner doesn't count
    CHECK(CatchUpLeaderPct(40.f, true, 140.f, out)); CHECK_EQ(out, 100.f);  // never past the finish
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(CatchUpLeaderPct(40.f, true, nan, out));   CHECK_EQ(out, 40.f);
    CHECK(!CatchUpLeaderPct(-1.f, true, 50.f, out));                        // no racer of ours
    CHECK(!CatchUpLeaderPct(nan, true, 50.f, out));
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

// ---- truck trailers -----------------------------------------------------------------------------
static MotionKey Key(double t, float x, float vx) {
    MotionKey k{};
    k.t = t; k.pos[0] = x; k.rot[3] = 1.f; k.linVel[0] = vx; k.angVel[1] = 0.5f;
    return k;
}

TEST(util_trailer_motion_ring) {
    MotionKey ring[4]; int n = 0;
    CHECK(PushMotionKey(ring, n, 4, Key(100, 0, 0)));
    CHECK(!PushMotionKey(ring, n, 4, Key(100, 1, 0)));   // duplicate time
    CHECK(!PushMotionKey(ring, n, 4, Key(50, 1, 0)));    // late packet
    CHECK_EQ(n, 1);
    for (int i = 1; i <= 5; ++i) CHECK(PushMotionKey(ring, n, 4, Key(100 + 50.0 * i, (float)i, 0)));
    CHECK_EQ(n, 4);                                       // full: the oldest went
    CHECK_NEAR(ring[0].t, 200, 1e-9);
    CHECK_NEAR(ring[3].t, 350, 1e-9);
    int bogus = 99;                                       // a broken count is reset, not used as an index
    CHECK(PushMotionKey(ring, bogus, 4, Key(1, 0, 0)));
    CHECK_EQ(bogus, 1);
}

TEST(util_trailer_sample_motion) {
    float p[3], q[4], v[3], w[3];
    CHECK(!SampleMotion(nullptr, 0, 0, p, q, v, w));
    MotionKey k[2] = { Key(1000, 0, 10), Key(1100, 1, 10) };   // 10 m/s, 1 m in 100 ms
    CHECK(SampleMotion(k, 2, 1050, p, q, v, w));
    CHECK_NEAR(p[0], 0.5, 1e-4);                          // halfway (Hermite with matching velocities = linear)
    CHECK_NEAR(v[0], 10, 1e-4);
    CHECK_NEAR(w[1], 0.5, 1e-5);
    CHECK_NEAR(q[3], 1, 1e-5);
    CHECK(SampleMotion(k, 2, 1150, p, q, v, w));          // 50 ms past the newest: moved on
    CHECK_NEAR(p[0], 1.5, 1e-4);
    CHECK_NEAR(v[0], 10, 1e-4);
    CHECK(SampleMotion(k, 2, 2000, p, q, v, w));          // long past: at most 250 ms on, then held still
    CHECK_NEAR(p[0], 3.5, 1e-4);
    CHECK_NEAR(v[0], 0, 1e-6);
    CHECK_NEAR(w[1], 0, 1e-6);
    CHECK(SampleMotion(k, 2, 500, p, q, v, w));           // before the history: the newest, not moved
    CHECK_NEAR(p[0], 1, 1e-4);
}

TEST(util_trailer_freshness_and_lost) {
    CHECK(!TrailerDataFresh(0, 0, 0));                    // no keys at all
    CHECK(TrailerDataFresh(2, 5000, 5000));
    CHECK(TrailerDataFresh(2, 4000, 5000));
    CHECK(!TrailerDataFresh(2, 3900, 5000));              // the host stopped sending this trailer
    float truck[3] = { 0, 0, 0 }, near_[3] = { 8, 0.5f, 3 }, far_[3] = { 20, 0, 0 };
    float nan_[3] = { std::numeric_limits<float>::quiet_NaN(), 0, 0 };
    CHECK(!TrailerLost(truck, near_));
    CHECK(TrailerLost(truck, far_));
    CHECK(TrailerLost(truck, nan_));
}

TEST(util_trailer_partner_match) {
    CHECK(TrailerMatches(-1, true) && TrailerMatches(-1, false));
    CHECK(TrailerMatches(1, true) && !TrailerMatches(1, false));
    CHECK(TrailerMatches(0, false) && !TrailerMatches(0, true));
    float at[3] = { 0, 0, 0 };
    TrafficCandidate c[3] = { { 0xA, { 2, 0, 0 }, false, true }, { 0xA, { 5, 0, 0 }, false }, { 0xB, { 1, 0, 0 }, false, true } };
    CHECK_EQ(PickTrafficPartner(0xA, at, c, 3, false), 0);         // not known: the nearest of the model
    CHECK_EQ(PickTrafficPartner(0xA, at, c, 3, false, 0), 1);      // a car without a trailer for one without
    CHECK_EQ(PickTrafficPartner(0xA, at, c, 3, false, 1), 0);
    CHECK_EQ(PickTrafficPartner(0xC, at, c, 3, true, 0), 1);       // any model, still without a trailer
    CHECK_EQ(PickTakeoverPartner(0xA, at, c, 3, 30.f, 10.f, 1), 0);
    CHECK_EQ(PickTakeoverPartner(0xC, at, c, 3, 30.f, 10.f, 1), 2); // any model: the nearest truck
    CHECK_EQ(PickTakeoverPartner(0xC, at, c, 3, 30.f, 10.f, 0), 1);
    TrafficCandidate none[1] = { { 0xA, { 1, 0, 0 }, false } };
    CHECK_EQ(PickTakeoverPartner(0xA, at, none, 1, 30.f, 30.f, 1), -1);  // a truck is never shown by a car without its trailer
}

TEST(util_trailer_entry_validation) {
    TrailerEntry e{};
    e.rot[3] = 1.f; e.flags = TRAILER_HITCHED | TRAILER_EMULATED;
    CHECK(ValidTrailerEntry(e));
    TrailerEntry b = e; b.flags = 4; CHECK(!ValidTrailerEntry(b));
    b = e; b.rot[3] = 0; CHECK(!ValidTrailerEntry(b));
    b = e; b.linVel[2] = 2e4f; CHECK(!ValidTrailerEntry(b));
    b = e; b.pos[1] = std::numeric_limits<float>::infinity(); CHECK(!ValidTrailerEntry(b));
}
