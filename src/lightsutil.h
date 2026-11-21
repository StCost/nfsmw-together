#pragma once
// Headlight flash ([Coop] FlashLights): the bits and numbers that don't need the game, so the test
// suite covers them (tests\test_lights.cpp). lights.inl reads the game and does what these say.
#include <cstdint>

namespace FlashLights {
    // PVehicle light bits (IVehicle+0x9C, VehicleFX::LightID): 1 / 2 / 4 left / right / centre head,
    // 8..0x20 brake, 0x40/0x80 reverse, 0x100..0x800 signals, 0x1000..0x4000 cop lights. The game's
    // lights table (0x905958) never uses the top bit, so it marks "flashing" here: it travels in
    // CarState::glare with the other bits, and an older build that gets it writes a bit nobody reads.
    constexpr uint32_t FLASH_BIT = 0x80000000u;
    constexpr uint32_t HEAD_BITS = 0x3u;          // left + right headlight (the centre one has no flare)
    constexpr int GAMEBREAKER_ACTION = 8;         // GAMEACTION_GAMEBREAKER (name table 0x8F6728)
    constexpr float HELD_THRESHOLD = 0.5f;        // the game's own press threshold for button bindings
    constexpr float FLARE_SCALE = 2.5f;           // headlight flare size while flashing
    // flare name hashes (CarRenderInfo flare +0x08) of the two drawn headlights
    constexpr uint32_t FLARE_LEFT_HEAD = 0x9DB90133u, FLARE_RIGHT_HEAD = 0xD09091C6u;

    // our car's light bits with the flash bit set / cleared
    inline uint32_t WithFlash(uint32_t glare, bool flashing) { return flashing ? (glare | FLASH_BIT) : (glare & ~FLASH_BIT); }
    inline bool IsFlashing(uint32_t glare) { return (glare & FLASH_BIT) != 0; }

    // The render packet's light bits (packet+0x60) after the game filled them from the car: a flashing car
    // has both headlights on (also in daylight) and keeps the flash bit for the flare pass; any other car
    // never carries the bit (the packet lives on between frames).
    inline uint32_t RenderBits(uint32_t packetBits, uint32_t carGlare) {
        return IsFlashing(carGlare) ? (packetBits | FLASH_BIT | HEAD_BITS) : (packetBits & ~FLASH_BIT);
    }

    inline bool IsHeadlightFlare(uint32_t nameHash) { return nameHash == FLARE_LEFT_HEAD || nameHash == FLARE_RIGHT_HEAD; }
    // size argument for the flare draw (0x505380, last argument): bigger while flashing, never 0 / NaN
    inline float FlareSize(float size, bool flashing) {
        if (!(size > 0.f) || size > 100.f) return size;   // leave odd values to the game
        return flashing ? size * FLARE_SCALE : size;
    }

    // One input binding as the game keeps it (+0x14 action id, +0x20 current value: 1 / 0 for buttons,
    // -1 without a device). The speedbreaker bind counts as held while any of its bindings is pressed.
    struct Binding { int action; float value; };
    inline bool BindHeld(const Binding* b, int n, int action = GAMEBREAKER_ACTION) {
        for (int i = 0; i < n; ++i)
            if (b[i].action == action && b[i].value >= HELD_THRESHOLD && b[i].value <= 2.f) return true;
        return false;
    }
}
