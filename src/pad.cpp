// Game controllers: hot-plug (a pad connected or reconnected while the game runs) and keeping the
// player's controller bindings.
//
// How speed.exe v1.3 handles controllers (from the code):
//  * Input library object L = *0x9BB410 (also *0x9203B4), created once by 0x628410 (GameDevice's first
//    constructor). L+0xB4 = IDirectInput8A. At start it enumerates ALL attached devices once
//    (0x7F1B26: EnumDevices(0, 0x7F1A6B, L, DIEDFL_ATTACHEDONLY)). The enum callback 0x7F1A6B
//    (stdcall(lpddi, L)) builds a device object per device type (0x14..0x18 = joystick / gamepad /
//    wheel ..: 0x7F2735, 0x4A4 bytes, CreateDevice + SetDataFormat(c_dfDIJoystick2) + Acquire, adds
//    itself to L's poll list via 0x7F1DAA) and hands it to the game's callback 0x6283D0, which appends
//    it to the joystick list 0x91F150[8] (count 0x9203B8). Nothing ever enumerates again, so a pad
//    connected later is never seen, and a pad that was unplugged keeps a dead device object.
//    Device object: +0x08 type (3 = joystick), +0x114 product name, +0x218 IDirectInputDevice8A.
//  * Force feedback library M = *0x9203C4 (created once by 0x628D30): its own two DirectInput devices
//    0x93DF00[2] (EnumDevices(GAMECTRL, 0x6D81D0) in 0x6DC590), M+0x4470[2] = the same devices.
//  * Joystick bindings: table 0x91F418 (0x34-byte entries; +0x1C / +0x28 = joystick binding 1 / 2).
//    0x63CD80 (thiscall GameDevice) writes the default joystick bindings for the pad in slot 0 (by the
//    hash of its product name). Callers: GameDevice init at start (vtable slot 3 0x63F850 ends with
//    "jmp 0x63CD80", return address 0x63CC72), the options screen's "restore defaults" (0x51D6E0, return
//    0x51D707) and the profile loader 0x56E200 (return 0x56E282).
//  * Profile: 0x56E180 saves the controller block, starting with 0x634DA0(GameDevice, 3) = hash of the
//    pad's product name (0x460BC0; no pad -> hash of "" = 0xFFFFFFFF). 0x56E200 loads it and, when the
//    saved hash differs from the current one, calls 0x63CD80 -> the joystick bindings are reset to the
//    current pad's defaults. Starting the game ONCE without the pad therefore resets the pad bindings to
//    the "no pad" defaults, and the next autosave stores them (with the "no pad" hash) for good.
//
// What this file does:
//  * Hot-plug ([Input] GamepadHotplug): a cheap check every 2 s (raw-input list of HID game controllers)
//    plus WM_DEVICECHANGE on the game window. When the set changes, enumerate the attached game
//    controllers with the game's own IDirectInput8 and
//      - feed each new one to the game's own enum callback 0x7F1A6B (exactly what happens at start),
//      - take devices that are no longer attached out of the joystick list (they stop being polled),
//      - the same for the force feedback library (0x6D81D0 + the per-slot setup of 0x6DC590/0x6DC790).
//    The player's bindings are kept. Only if the current bindings are the "no pad" defaults (no pad at
//    start and nothing else loaded) the new pad gets its defaults, as if it had been there at start.
//  * Bindings ([Input] KeepPadBindings): while no pad is attached, 0x634DA0 answers with the pad the
//    bindings were made for, so loading the profile without the pad keeps them and saving keeps the pad
//    identity. A reset of the joystick bindings from anywhere other than the three known callers is
//    blocked and logged; a second run of the full defaults copy 0x628410 keeps the table.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <intrin.h>
#include <cstring>
#include <cstdint>
#include "input.h"
#include "config.h"
#include "log.h"
#include "mem.h"

#pragma intrinsic(_ReturnAddress)

namespace {
    constexpr uint32_t HASH_NONE = 0xFFFFFFFFu;   // 0x460BC0("") - the "no pad" hash

    // ---- addresses (speed.exe v1.3) ----
    constexpr uintptr_t A_InputLib      = 0x9BB410;  // input library object L
    constexpr uintptr_t A_JoyList       = 0x91F150;  // joystick device objects [8]
    constexpr uintptr_t A_JoyCount      = 0x9203B8;
    constexpr uintptr_t A_JoyBaseline   = 0x91F1E8;  // copy of pad 0's state at init (0x8C dwords)
    constexpr uintptr_t A_GameDevCount  = 0x9203C0;  // live GameDevice objects
    constexpr uintptr_t A_BindTable     = 0x91F418;
    constexpr size_t    BindTableSize   = 0xF70;
    constexpr uintptr_t A_FFLib         = 0x9203C4;  // force feedback library M
    constexpr uintptr_t A_FFDevs        = 0x93DF00;  // [2] IDirectInputDevice8A*
    constexpr uintptr_t A_FFFlagJoy     = 0x982904;  // [2] DI8DEVTYPE_JOYSTICK
    constexpr uintptr_t A_FFFlagWheel   = 0x93CC58;  // [2] DI8DEVTYPE_DRIVING
    constexpr uintptr_t A_FFFlagPad     = 0x93D904;  // [2] DI8DEVTYPE_GAMEPAD
    constexpr uintptr_t A_FFAxes        = 0x93CC34;  // [2] axis count
    constexpr uintptr_t A_FFHwnd        = 0x982BF4;
    constexpr uintptr_t A_JoyFormat     = 0x8C152C;  // c_dfDIJoystick2 inside speed.exe

    using EnumCb_t      = BOOL(__stdcall*)(LPCDIDEVICEINSTANCEA, void*);
    using Thiscall0     = void(__thiscall*)(void*);
    using PadHash_t     = uint32_t(__thiscall*)(void*, int);
    using LoadCtl_t     = uint8_t*(__cdecl*)(uint8_t*, uint8_t*);
    using Cdecl0        = void(__cdecl*)();

    PadHash_t  o_PadHash = (PadHash_t)0x634DA0;
    LoadCtl_t  o_LoadControls = nullptr;
    Cdecl0     o_InitTables = nullptr;
    Thiscall0  g_defaultsTramp = nullptr;   // the first 5 bytes of 0x63CD80 + jmp back

    uint32_t g_bindingsFor = 0;      // pad hash the joystick bindings are made for (0 = not known yet)
    bool     g_inLoad = false, g_loadHaveSaved = false, g_defaultsInLoad = false;
    uint32_t g_loadSaved = 0;
    bool     g_allowDefaults = false; // our own call (new pad on "no pad" bindings)

    void* GameDevice() {
        uint8_t* scanner = ((uint8_t*(__cdecl*)())0x634370)();
        void* gd = nullptr;
        Mem::Read((uintptr_t)scanner + 8, gd);
        return gd;
    }
    void* FirstPad() { void* p = nullptr; Mem::Read(A_JoyList, p); return p; }

    // ---- bindings guard -------------------------------------------------------------------------
    uint32_t __fastcall hk_PadHash(void* self, void*, int type) {
        uint32_t r = o_PadHash(self, type);
        if (type != 3 || !g_cfg.keepPadBindings || FirstPad()) return r;
        if (g_inLoad) {
            // loading the profile without the pad: answer with the saved pad so its bindings stay
            if (g_loadHaveSaved && g_loadSaved != r) {
                LOG("Pad: no controller attached - keeping the profile's controller bindings (made for %08X)", g_loadSaved);
                return g_loadSaved;
            }
            return r;
        }
        // saving without the pad: keep the pad the bindings belong to
        if (g_bindingsFor && g_bindingsFor != HASH_NONE) return g_bindingsFor;
        return r;
    }

    uint8_t* __cdecl hk_LoadControls(uint8_t* buf, uint8_t* end) {
        g_loadHaveSaved = buf && end && buf + 4 <= end && Mem::Read((uintptr_t)buf, g_loadSaved);
        g_inLoad = true;
        g_defaultsInLoad = false;
        uint8_t* r = o_LoadControls(buf, end);
        g_inLoad = false;
        if (g_loadHaveSaved && !g_defaultsInLoad) g_bindingsFor = g_loadSaved;
        LOG("Pad: controller settings loaded from the profile (bindings for %08X%s)", g_bindingsFor,
            g_defaultsInLoad ? ", reset to this pad's defaults: the profile was saved with another controller" : "");
        return r;
    }

    void __fastcall hk_JoyDefaults(void* self, void*) {
        uintptr_t from = (uintptr_t)_ReturnAddress();
        bool known = from == 0x63CC72 || from == 0x51D707 || from == 0x56E282;
        if (g_cfg.keepPadBindings && !known && !g_allowDefaults) {
            LOG("Pad: blocked a reset of the controller bindings (called from %08X)", (unsigned)from);
            return;
        }
        g_defaultsTramp(self);
        g_bindingsFor = o_PadHash(self, 3);
        if (g_inLoad) g_defaultsInLoad = true;
        LOG("Pad: controller bindings set to the defaults for %08X (called from %08X)", g_bindingsFor, (unsigned)from);
    }

    void __cdecl hk_InitTables() {
        static int runs = 0;
        if (++runs == 1 || !g_cfg.keepPadBindings) { o_InitTables(); return; }
        static uint8_t keep[BindTableSize];
        bool saved = Mem::SafeRead((const void*)A_BindTable, keep, BindTableSize);
        o_InitTables();
        if (saved) Mem::SafeWrite((void*)A_BindTable, keep, BindTableSize);
        LOG("Pad: the game re-created its input devices (run %d) - controller bindings kept", runs);
    }

    bool PatchCallChecked(uintptr_t at, uintptr_t expectTarget, void* to, uintptr_t* orig) {
        uint8_t op = 0; int32_t rel = 0;
        if (!Mem::Read(at, op) || !Mem::Read(at + 1, rel) || op != 0xE8 || at + 5 + rel != expectTarget) {
            LOG("Pad: call at %08X is not the expected one - not patched", (unsigned)at);
            return false;
        }
        uintptr_t old = Mem::PatchCall(at, to);
        if (orig) *orig = old;
        return true;
    }

    // ---- hot-plug -------------------------------------------------------------------------------
    DIDEVICEINSTANCEA g_found[16];
    int g_nFound = 0;

    BOOL CALLBACK CollectCb(LPCDIDEVICEINSTANCEA d, LPVOID) {
        uint8_t t = (uint8_t)(d->dwDevType & 0xFF);
        if (t >= 0x14 && t <= 0x18 && g_nFound < 16) g_found[g_nFound++] = *d;   // the game's own type filter
        return DIENUM_CONTINUE;
    }

    bool Attached(const GUID& g) {
        for (int i = 0; i < g_nFound; ++i) if (IsEqualGUID(g_found[i].guidInstance, g)) return true;
        return false;
    }

    bool DeviceGuid(IDirectInputDevice8A* d, GUID& out) {
        if (!d) return false;
        DIDEVICEINSTANCEA inst{};
        inst.dwSize = sizeof(inst);
        if (FAILED(d->GetDeviceInfo(&inst))) return false;
        out = inst.guidInstance;
        return true;
    }

    // Signature of the attached HID game controllers (joystick / gamepad / multi-axis). Cheap: no
    // driver I/O, just the raw-input device list.
    uint32_t PadSignature() {
        RAWINPUTDEVICELIST list[64];
        UINT n = 64;
        UINT got = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
        if (got == (UINT)-1) return 0;
        uint32_t sig = 2166136261u, cnt = 0;
        for (UINT i = 0; i < got; ++i) {
            if (list[i].dwType != RIM_TYPEHID) continue;
            RID_DEVICE_INFO info{};
            info.cbSize = sizeof(info);
            UINT sz = sizeof(info);
            if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1) continue;
            USHORT page = info.hid.usUsagePage, usage = info.hid.usUsage;
            if (page != 1 || (usage != 4 && usage != 5 && usage != 8)) continue;
            sig = (sig ^ (uint32_t)(uintptr_t)list[i].hDevice) * 16777619u;
            ++cnt;
        }
        return sig ^ (cnt << 24);
    }

    // Main input library: drop dead pads, add new ones through the game's own enum callback.
    // Returns true if pad 0 changed.
    bool RefreshInputLib(uint8_t* L) {
        int count = 0;
        Mem::Read(A_JoyCount, count);
        if (count < 0 || count > 8) return false;
        void* list[8] = {};
        Mem::SafeRead((const void*)A_JoyList, list, sizeof(list));
        void* first = list[0];

        // 1. devices no longer attached: stop polling them, take them out of the list (the object is
        //    left allocated - nothing frees it safely while the game might still look at it)
        int kept = 0;
        for (int i = 0; i < count; ++i) {
            uint8_t* dev = (uint8_t*)list[i];
            if (!dev) continue;
            int type = 0; IDirectInputDevice8A* did = nullptr; GUID g{};
            Mem::Read((uintptr_t)dev + 8, type);
            Mem::Read((uintptr_t)dev + 0x218, did);
            bool alive = type != 3 || (DeviceGuid(did, g) && Attached(g));
            if (alive) { list[kept++] = dev; continue; }
            char name[64] = "";
            Mem::SafeRead(dev + 0x114, name, sizeof(name) - 1);
            LOG("Pad: controller %d disconnected (%s) - removed from the game's list", i, name);
            ((void(__thiscall*)(void*, void*))0x7F1DF1)(L, dev);      // L: remove from the poll list
            if (did) did->Unacquire();
        }
        for (int i = kept; i < 8; ++i) list[i] = nullptr;
        if (kept != count) {
            Mem::SafeWrite((void*)A_JoyList, list, sizeof(list));
            Mem::Write(A_JoyCount, kept);
            count = kept;
        }

        // 2. attached devices the game doesn't have: the game's own enum callback creates and adds them
        for (int f = 0; f < g_nFound; ++f) {
            bool have = false;
            for (int i = 0; i < count && !have; ++i) {
                IDirectInputDevice8A* did = nullptr; GUID g{};
                Mem::Read((uintptr_t)list[i] + 0x218, did);
                have = DeviceGuid(did, g) && IsEqualGUID(g, g_found[f].guidInstance);
            }
            if (have) continue;
            if (count >= 8) { LOG("Pad: the game's controller list is full - %s not added", g_found[f].tszProductName); break; }
            ((EnumCb_t)0x7F1A6B)(&g_found[f], L);
            int now = 0;
            Mem::Read(A_JoyCount, now);
            Mem::SafeRead((const void*)A_JoyList, list, sizeof(list));
            LOG("Pad: controller connected: %s -> %s", g_found[f].tszProductName,
                now > count ? "added to the game" : "the game did not take it");
            count = now;
        }
        return FirstPad() != first;
    }

    // Force feedback library: same idea for its two device slots.
    void RefreshFFLib() {
        uint8_t* M = nullptr;
        if (!Mem::Read(A_FFLib, M) || !M) return;
        HWND hwnd = nullptr;
        Mem::Read(A_FFHwnd, hwnd);
        for (int i = 0; i < 2; ++i) {
            IDirectInputDevice8A* d = nullptr; GUID g{};
            Mem::Read(A_FFDevs + i * 4, d);
            if (!d || (DeviceGuid(d, g) && Attached(g))) continue;
            d->Unacquire();     // the object and its effects are left alone (see above)
            int zero = 0;
            Mem::Write(A_FFDevs + i * 4, zero);
            Mem::Write((uintptr_t)M + 0x4470 + i * 4, zero);
            Mem::Write(A_FFFlagJoy + i * 4, zero); Mem::Write(A_FFFlagWheel + i * 4, zero); Mem::Write(A_FFFlagPad + i * 4, zero);
            Mem::Write(A_FFAxes + i * 4, zero);
            uint8_t* sub = M + 0x2228;
            Mem::Write((uintptr_t)sub + 0x2220 + i * 4, zero); Mem::Write((uintptr_t)sub + 0x2228 + i * 4, zero);
            Mem::Write((uintptr_t)sub + 0x2230 + i * 4, zero); Mem::Write((uintptr_t)sub + 0x2250 + i * 4, zero);
            Mem::Write((uintptr_t)sub + 0x2478 + i * 4, zero);
            static const uint8_t z[0x20] = {};
            const uintptr_t effectArrays[] = { 0x4728, 0x4768, 0x47A8, 0x47E8, 0x4828, 0x4868, 0x48E8 };
            for (uintptr_t a : effectArrays) Mem::SafeWrite(M + a + i * 0x20, z, sizeof(z));
            LOG("Pad: force feedback slot %d released (controller gone)", i);
        }
        for (int f = 0; f < g_nFound; ++f) {
            int freeSlot = -1; bool have = false;
            for (int i = 0; i < 2; ++i) {
                IDirectInputDevice8A* d = nullptr; GUID g{};
                Mem::Read(A_FFDevs + i * 4, d);
                if (!d) { if (freeSlot < 0) freeSlot = i; continue; }
                if (DeviceGuid(d, g) && IsEqualGUID(g, g_found[f].guidInstance)) have = true;
            }
            if (have || freeSlot < 0) continue;
            ((EnumCb_t)0x6D81D0)(&g_found[f], nullptr);    // the game's callback: CreateDevice + type flags
            IDirectInputDevice8A* d = nullptr;
            Mem::Read(A_FFDevs + freeSlot * 4, d);
            if (!d) continue;
            int i = freeSlot;
            uint8_t* sub = M + 0x2228;
            // what 0x6DC590 does after its EnumDevices, for this slot
            int v = 0;
            Mem::Read(A_FFFlagJoy + i * 4, v);   Mem::Write((uintptr_t)sub + 0x2220 + i * 4, v);
            Mem::Read(A_FFFlagWheel + i * 4, v); Mem::Write((uintptr_t)sub + 0x2228 + i * 4, v);
            Mem::Read(A_FFFlagPad + i * 4, v);   Mem::Write((uintptr_t)sub + 0x2230 + i * 4, v);
            int zero = 0;
            Mem::Write(A_FFAxes + i * 4, zero);
            d->EnumObjects((LPDIENUMDEVICEOBJECTSCALLBACKA)0x6C9E30, (void*)(A_FFAxes + i * 4), DIDFT_AXIS);
            Mem::Read(A_FFAxes + i * 4, v);      Mem::Write((uintptr_t)sub + 0x2250 + i * 4, v);
            DIDEVICEINSTANCEA inst{}; inst.dwSize = sizeof(inst);
            if (SUCCEEDED(d->GetDeviceInfo(&inst))) {
                int one = 1;
                if (inst.guidProduct.Data1 == 0xC291046D) Mem::Write((uintptr_t)sub + (i == 0 ? 0x2238 : 0x223C), one);
                else if (i == 0 && inst.guidProduct.Data1 == 0xC218046D) Mem::Write((uintptr_t)sub + 0x2240, one);
            }
            // what 0x6DC790 / 0x6D81F0 do for it: data format, cooperative level, axis ranges
            HRESULT h1 = d->SetDataFormat((LPCDIDATAFORMAT)A_JoyFormat);
            HRESULT h2 = d->SetCooperativeLevel(hwnd, DISCL_EXCLUSIVE | DISCL_FOREGROUND);
            d->EnumObjects((LPDIENUMDEVICEOBJECTSCALLBACKA)0x6D82F0, d, DIDFT_ALL);
            Mem::Write((uintptr_t)M + 0x4470 + i * 4, d);
            if (((int(__thiscall*)(void*, int))0x6CA110)(M, i))
                ((void(__thiscall*)(void*, int, int, int, int))0x6D8680)(M, i, 0, 0xB4, 0xB4);   // as 0x628D30
            LOG("Pad: force feedback slot %d: %s (format %08X, coop %08X)", i, g_found[f].tszProductName, (unsigned)h1, (unsigned)h2);
        }
    }

    // Pad 0 changed: what GameDevice init (0x63F850) does for it, minus the bindings reset.
    void OnFirstPadChanged() {
        uint8_t* pad = (uint8_t*)FirstPad();
        if (pad) {
            void** vt = nullptr;
            Mem::Read((uintptr_t)pad, vt);
            ((int(__thiscall*)(void*))vt[4])(pad);                       // poll
            uint8_t* state = ((uint8_t*(__thiscall*)(void*))vt[1])(pad); // current state
            if (state) Mem::SafeWrite((void*)A_JoyBaseline, state, 0x8C * 4);
            uint32_t h = ((uint32_t(__cdecl*)(const char*))0x460BC0)((const char*)pad + 0x114);
            if (h == 0x5FB6CCBA) { uint32_t ff = 0xFFFF; Mem::Write(0x91F1EC, ff); Mem::Write(0x91F3FC, ff); }
        } else {
            static const uint8_t z[0x8C * 4] = {};
            Mem::SafeWrite((void*)A_JoyBaseline, z, sizeof(z));
        }
        ((void(__cdecl*)())0x6282E0)();                                  // pad-0 axis flag (0x9203BC)
        if (!pad) return;
        void* gd = GameDevice();
        uint32_t padHash = gd ? o_PadHash(gd, 3) : 0;
        if (gd && g_bindingsFor == HASH_NONE) {
            // bindings are the "no pad" defaults: give the new pad its defaults, as at start
            g_allowDefaults = true;
            ((Thiscall0)0x63CD80)(gd);
            g_allowDefaults = false;
        } else {
            LOG("Pad: keeping the controller bindings (made for %08X, this pad %08X)", g_bindingsFor, padHash);
        }
    }

    int RescanRaw() {
        __try {
            uint8_t* L = nullptr;
            if (!Mem::Read(A_InputLib, L) || !L) return 0;
            IDirectInput8A* di = nullptr;
            if (!Mem::Read((uintptr_t)L + 0xB4, di) || !di) return 0;
            g_nFound = 0;
            if (FAILED(di->EnumDevices(DI8DEVCLASS_GAMECTRL, CollectCb, nullptr, DIEDFL_ATTACHEDONLY))) return 0;
            bool firstChanged = RefreshInputLib(L);
            RefreshFFLib();
            if (firstChanged) OnFirstPadChanged();
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    volatile LONG g_devChangeAt = 0;
    uint32_t g_sig = 0;
    bool     g_sigInit = false;
    DWORD    g_lastCheck = 0, g_lastScan = 0;
}

bool Pad::InstallHooks() {
    bool ok = true;
    if (g_cfg.keepPadBindings) {
        uintptr_t orig = 0;
        ok &= PatchCallChecked(0x56E190, 0x634DA0, (void*)&hk_PadHash, nullptr);   // save
        ok &= PatchCallChecked(0x56E26A, 0x634DA0, (void*)&hk_PadHash, nullptr);   // load
        if (PatchCallChecked(0x5ACD30, 0x56E200, (void*)&hk_LoadControls, &orig)) o_LoadControls = (LoadCtl_t)orig; else ok = false;
        if (PatchCallChecked(0x634BFA, 0x628410, (void*)&hk_InitTables, &orig)) o_InitTables = (Cdecl0)orig; else ok = false;
    }
    // 0x63CD80 starts with "mov eax, [0x91F150]" (5 bytes): replace with a jmp, the trampoline runs it.
    // Hooked even with KeepPadBindings off: hot-plug uses it to know what the bindings are made for.
    static const uint8_t expect[5] = { 0xA1, 0x50, 0xF1, 0x91, 0x00 };
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (tramp && memcmp((const void*)0x63CD80, expect, 5) == 0) {
        memcpy(tramp, expect, 5);
        tramp[5] = 0xE9;
        *(int32_t*)(tramp + 6) = (int32_t)(0x63CD85 - ((uintptr_t)tramp + 10));
        FlushInstructionCache(GetCurrentProcess(), tramp, 10);
        g_defaultsTramp = (Thiscall0)tramp;
        uint8_t jmp[5] = { 0xE9 };
        *(int32_t*)(jmp + 1) = (int32_t)((uintptr_t)&hk_JoyDefaults - (0x63CD80 + 5));
        ok &= Mem::PatchBytes(0x63CD80, expect, jmp, 5);
    } else {
        ok = false;
    }
    LOG("Pad: controller bindings guard %s, hot-plug %s", ok ? "installed" : "NOT fully installed",
        g_cfg.gamepadHotplug ? "on" : "off");
    return ok;
}

void Pad::OnDeviceChange() {
    InterlockedExchange(&g_devChangeAt, (LONG)(GetTickCount() | 1));
}

void Pad::Tick(int flow) {
    if (!g_cfg.gamepadHotplug) return;
    if (flow != 3 && flow != 6) return;                       // not while loading
    int gd = 0;
    if (!Mem::Read(A_GameDevCount, gd) || gd <= 0) return;
    void* L = nullptr;
    if (!Mem::Read(A_InputLib, L) || !L) return;
    DWORD now = GetTickCount();
    bool scan = false;
    LONG dc = g_devChangeAt;
    if (dc && now - (DWORD)dc > 700) { InterlockedExchange(&g_devChangeAt, 0); scan = true; }   // let the driver settle
    if (now - g_lastCheck >= 2000) {
        g_lastCheck = now;
        uint32_t sig = PadSignature();
        if (!g_sigInit) { g_sigInit = true; g_sig = sig; scan = true; }   // once: catch anything since start
        else if (sig != g_sig) { g_sig = sig; scan = true; }
    }
    if (!scan || now - g_lastScan < 1000) return;
    uint8_t* gdev = (uint8_t*)GameDevice();
    uint8_t remap = 0;
    if (gdev && Mem::Read((uintptr_t)gdev + 0x183C, remap) && remap) return;   // the player is binding a key right now
    g_lastScan = now;
    int r = RescanRaw();
    if (r < 0) LOG("Pad: exception while checking controllers");
}
