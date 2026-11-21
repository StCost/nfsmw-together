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
