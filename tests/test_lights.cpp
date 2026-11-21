// Headlight flash (src/lightsutil.h): the flash bit, the render bits, flare size, the held bind, and the
// bit travelling in the state packet.
#include "net_helpers.h"
#include "../src/lightsutil.h"
#include <limits>

using namespace FlashLights;

TEST(lights_flash_bit) {
    CHECK_EQ(WithFlash(0x7, true), 0x80000007u);
    CHECK_EQ(WithFlash(0x80000007u, false), 0x7u);
    CHECK_EQ(WithFlash(0, false), 0u);
    CHECK(IsFlashing(FLASH_BIT | 0x38)); CHECK(!IsFlashing(0x7FFFFFFF));
    // the bit is none of the game's light bits (head 1/2/4 .. cop lights 0x4000)
    CHECK_EQ(FLASH_BIT & 0x7FFF, 0u);
}

TEST(lights_render_bits) {
    // a flashing car: both headlights on (also when the game had them off) and the flash bit kept
    CHECK_EQ(RenderBits(0x38, FLASH_BIT), 0x38u | HEAD_BITS | FLASH_BIT);
    CHECK_EQ(RenderBits(0x3, FLASH_BIT | 0x3), 0x3u | FLASH_BIT);
    // not flashing: the bit is cleared from a packet that had it last frame, the rest untouched
    CHECK_EQ(RenderBits(FLASH_BIT | 0x3, 0x3), 0x3u);
    CHECK_EQ(RenderBits(0x0, 0x0), 0u);
    CHECK_EQ(RenderBits(0x1000, 0x1000), 0x1000u);
}

TEST(lights_flare_size) {
    CHECK(IsHeadlightFlare(FLARE_LEFT_HEAD)); CHECK(IsHeadlightFlare(FLARE_RIGHT_HEAD));
    CHECK(!IsHeadlightFlare(0x7A5BCF69u));            // centre headlight (no flare drawn)
    CHECK(!IsHeadlightFlare(0));
    CHECK_NEAR(FlareSize(1.f, false), 1.f, 1e-6);
    CHECK_NEAR(FlareSize(1.f, true), FLARE_SCALE, 1e-6);
    CHECK_NEAR(FlareSize(10.f, true), 10.f * FLARE_SCALE, 1e-4);
    CHECK_NEAR(FlareSize(0.f, true), 0.f, 1e-6);      // odd values are left alone
    CHECK_NEAR(FlareSize(-1.f, true), -1.f, 1e-6);
    CHECK_NEAR(FlareSize(1000.f, true), 1000.f, 1e-6);
    float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(FlareSize(nan, true) != FlareSize(nan, true));   // still NaN, not scaled into something else
}

TEST(lights_bind_held) {
    Binding none[1] = { { 3, 1.f } };
    CHECK(!BindHeld(none, 1));                          // another action pressed
    Binding b[3] = { { 11, 1.f }, { GAMEBREAKER_ACTION, 0.f }, { GAMEBREAKER_ACTION, -1.f } };
    CHECK(!BindHeld(b, 3));                             // released keyboard key, pad without a device
    b[2].value = 1.f;
    CHECK(BindHeld(b, 3));                              // the pad button is held
    b[2].value = 0.49f;
    CHECK(!BindHeld(b, 3));
    b[2].value = std::numeric_limits<float>::quiet_NaN();
    CHECK(!BindHeld(b, 3));
    b[2].value = 1e9f;
    CHECK(!BindHeld(b, 3));                             // garbage, not a button
    CHECK(!BindHeld(nullptr, 0));
    CHECK(BindHeld(b, 1, 11));                          // any action id can be asked
}

TEST(lights_flash_travels_in_the_state) {
    th::NetFixture client(false);
    th::Connect("Host");
    StatePacket p = th::MakeState(1, 10.f, 0.f, 0.f);
    p.s.glare = WithFlash(0x3, true);
    th::Inject(p);
    REQUIRE(Net::Latest().valid);
    CHECK(IsFlashing(Net::Latest().s.glare));
    CHECK_EQ(Net::Latest().s.glare & 0x7FFFFFFF, 0x3u);
}
