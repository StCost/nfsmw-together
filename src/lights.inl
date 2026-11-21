// Included from game.cpp (after milestone.inl).
// ---------------------------------------------------------------------------
// Headlight flash ([Coop] FlashLights): in co-op the speedbreaker is off, so holding its key / button
// flashes the car's headlights instead (bigger, brighter headlight flares), on both screens. Purely visual.
// Decisions and numbers: lightsutil.h (tested).
//
// Game side (speed.exe v1.3, read from the code):
//  Input: the human car's IInputPlayer (UCOM handle 0x403F80, vtable 0x8AC658; FetchInput 0x6A8870 is
//    slot 4). Its action queue sits at +0x10; queue+0x270 = the bindings object, +4 = list head, nodes
//    {next, prev, +0x08 trigger type, +0x14 action id, +0x20 current value}. The poll 0x642C90 writes
//    1 / 0 (button) into +0x20 every frame. GAMEACTION_GAMEBREAKER = 8; its binding only queues presses
//    (FetchInput case 5, 0x6A8A5D, toggles), so "held" is read from the binding value itself.
//  Lights: PVehicle light bits at IVehicle+0x9C. The car render update 0x6B6FB0 (thiscall, this+0x90 =
//    IVehicle, packet arg; vtable entries 0x8AD868 / 0x8AD958) turns them into packet+0x60 through the
//    lights table 0x905958; CarRenderConn copies that to CarRenderInfo+0x1620 (0x7564C0).
//    CarRenderInfo::RenderFlaresOnCar 0x742950 (called at 0x744EEA / 0x7450E8, ecx = CarRenderInfo) draws
//    each flare through eRenderLightFlare 0x505380 (cdecl, 9 args, called at 0x743235): arg 2 = the flare
//    (+0x08 name hash), arg 9 = size scale. Headlight intensity is already the maximum (1.0, clamped),
//    so a flash is the headlight flare drawn a second time, 2.5x as big.
// The flash travels as bit 0x80000000 of our light bits (CarState::glare), which the ghost car gets.
// ---------------------------------------------------------------------------
namespace {
    namespace FL = FlashLights;
    constexpr uintptr_t InputPlayerHandle = 0x403F80, VT_InputPlayer = 0x8AC658;
    constexpr uintptr_t RenderUpdateFn = 0x6B6FB0;
    constexpr uintptr_t RenderUpdateSlots[2] = { 0x8AD868, 0x8AD958 };
    constexpr uintptr_t RenderFlaresFn = 0x742950, RenderFlaresCall1 = 0x744EEA, RenderFlaresCall2 = 0x7450E8;
    constexpr uintptr_t LightFlareFn = 0x505380, LightFlareCall = 0x743235;

    bool     g_flashHooked = false;
    bool     g_flashing = false;          // our car flashes right now
    DWORD    g_flashTestUntil = 0;        // test command `flash`
    uint8_t* volatile g_flareInfo = nullptr;   // the CarRenderInfo whose flares are drawn right now
    uintptr_t g_renderFlaresFn = RenderFlaresFn;
    int      g_flashPresses = 0;

    void FlashPacketSafe(uint8_t* self, uint8_t* packet) {
        __try {
            uint8_t* iv = *(uint8_t**)(self + 0x90);
            uint32_t glare = (iv && VTable(iv) == Addr::VT_PVehicle_IVehicle) ? *(const uint32_t*)(iv + 0x9C) : 0;
            uint32_t& bits = *(uint32_t*)(packet + 0x60);
            bits = FL::RenderBits(bits, glare);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    void __fastcall hk_RenderUpdate(uint8_t* self, void*, uint8_t* packet) {
        ((void(__thiscall*)(void*, void*))RenderUpdateFn)(self, packet);
        if (g_flashHooked) FlashPacketSafe(self, packet);
    }
    // the two RenderFlaresOnCar calls: remember which car, then the game's function (same stack)
    __declspec(naked) void hk_RenderFlares() {
        __asm {
            mov g_flareInfo, ecx
            jmp dword ptr [g_renderFlaresFn]
        }
    }
    bool FlareFlashesSafe(const uint8_t* flare) {
        __try {
            const uint8_t* info = g_flareInfo;
            if (!info || !flare || !FL::IsFlashing(*(const uint32_t*)(info + 0x1620))) return false;
            return FL::IsHeadlightFlare(*(const uint32_t*)(flare + 8));
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    using LightFlareFnT = void(__cdecl*)(uint32_t, uint8_t*, uint32_t, float, uint32_t, uint32_t, uint32_t, uint32_t, float);
    void __cdecl hk_LightFlare(uint32_t a1, uint8_t* flare, uint32_t a3, float intensity, uint32_t a5, uint32_t a6,
                               uint32_t a7, uint32_t a8, float size) {
        ((LightFlareFnT)LightFlareFn)(a1, flare, a3, intensity, a5, a6, a7, a8, size);
        if (FlareFlashesSafe(flare))
            ((LightFlareFnT)LightFlareFn)(a1, flare, a3, intensity, a5, a6, a7, a8, FL::FlareSize(size, true));
    }

    // Is the speedbreaker key / button held on our car right now? (raw: call inside __try)
    bool SpeedbreakerHeldRaw(void* iv) {
        uint8_t* ip = (uint8_t*)Find(iv, InputPlayerHandle);
        if (!ip || VTable(ip) != VT_InputPlayer) return false;
        uint8_t* binds = *(uint8_t**)(ip + 0x10 + 0x270);
        if (!binds) return false;
        uint8_t* head = *(uint8_t**)(binds + 4);
        if (!head) return false;
        FL::Binding b[256]; int n = 0;
        for (uint8_t* node = *(uint8_t**)head; node && node != head && n < 256; node = *(uint8_t**)node) {
            b[n].action = *(const int*)(node + 0x14);
            b[n].value = *(const float*)(node + 0x20);
            ++n;
        }
        return FL::BindHeld(b, n);
    }
    bool CallTargetIs(uintptr_t site, uintptr_t fn) {
        uint8_t op = 0; int32_t rel = 0;
        return Mem::Read(site, op) && Mem::Read(site + 1, rel) && op == 0xE8 && site + 5 + rel == fn;
    }
}

// Every frame with our car (from ReadLocalCar, inside its __try): set or clear the flash bit on it.
static void FlashLightsTick(void* iv) {
    if (!g_flashHooked || !iv) return;
    DWORD now = GetTickCount();
    bool want = false;
    __try {
        want = g_cfg.flashLights && g_cfg.noSpeedbreaker && g_coopLinked && Net::Stats().connected && !NisActive() &&
               ((int32_t)(g_flashTestUntil - now) > 0 || SpeedbreakerHeldRaw(iv));
        uint32_t* bits = (uint32_t*)((uint8_t*)iv + 0x9C);
        *bits = FL::WithFlash(*bits, want);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (want != g_flashing) {
        g_flashing = want;
        if (want && (++g_flashPresses <= 5 || g_flashPresses % 50 == 0)) LOG("Lights: flashing our headlights (speedbreaker bind held, %d so far)", g_flashPresses);
    }
}
// the other player's car: say when their flash shows here (first few times)
static void FlashSeenOnGhost(uint32_t glare) {
    static bool last = false; static int n = 0;
    bool on = FL::IsFlashing(glare);
    if (on && !last && g_flashHooked && (++n <= 5 || n % 50 == 0)) LOG("Lights: the other player flashes their headlights (%d so far)", n);
    last = on;
}

bool Game::InstallFlashLights() {
    if (!g_cfg.flashLights) { LOG("Lights: FlashLights=0 - the speedbreaker bind does nothing in co-op"); return false; }
    uintptr_t s0 = 0, s1 = 0;
    bool ok = Mem::Read(RenderUpdateSlots[0], s0) && Mem::Read(RenderUpdateSlots[1], s1) && s0 == RenderUpdateFn && s1 == RenderUpdateFn &&
              CallTargetIs(RenderFlaresCall1, RenderFlaresFn) && CallTargetIs(RenderFlaresCall2, RenderFlaresFn) &&
              CallTargetIs(LightFlareCall, LightFlareFn);
    if (!ok) { LOG("Lights: headlight flash NOT installed (game code differs)"); return false; }
    for (uintptr_t slot : RenderUpdateSlots) Mem::PatchPointer((void*)slot, (void*)&hk_RenderUpdate);
    Mem::PatchCall(RenderFlaresCall1, (void*)&hk_RenderFlares);
    Mem::PatchCall(RenderFlaresCall2, (void*)&hk_RenderFlares);
    Mem::PatchCall(LightFlareCall, (void*)&hk_LightFlare);
    g_flashHooked = true;
    LOG("Lights: hold the speedbreaker bind to flash the headlights in co-op (render hooks installed)");
    return true;
}
std::string Game::DebugFlash() {
    if (!g_flashHooked) return "headlight flash not installed";
    g_flashTestUntil = GetTickCount() + 5000;
    return "flashing our headlights for 5 s (only while co-op is on)";
}
