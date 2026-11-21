#include "game.h"
#include "netutil.h"
#include "pursuitutil.h"
#include "mem.h"
#include "log.h"
#include "config.h"
#include "input.h"
#include "overlay.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <shlobj.h>
#include <vector>
#include <string>
#include <share.h>

// ---------------------------------------------------------------------------
// speed.exe v1.3 addresses. Each one was checked against the exe's own code:
//  * game flow:   `cmp dword ptr [0x925E90], 6` appears ~150 times
//  * UCOM Find:   0x5D59F0 (thiscall Object*, handle); handles are functions that
//                 `mov eax, <own address>; ret` (0x4039F0 / 0x4039E0 / 0x4040E0)
//  * vtables:     taken from the constructors that register each interface
//                 (RBVehicle ctor @0x6B7B1A, PVehicle ctor @0x689072)
//  * vtable slot meaning: disassembled (e.g. slot 23 copies 3 floats to +0x10)
// Reference: nya-common-nfsmw / MostWantedTimeTrialGhosts (MPL-2.0), ExOpts.
// ---------------------------------------------------------------------------
namespace Addr {
    constexpr uintptr_t EntryPoint     = 0x7C4040;
    constexpr uintptr_t GameFlowState  = 0x925E90; // 3 = front end, 4/5 = loading, 6 = in game
    constexpr uintptr_t VehicleLists   = 0x92CD18; // UTL::ListableSet<IVehicle,20,eVehicleList,10>
    constexpr uint32_t  VehicleBucket  = 0x60;     // FixedVector: vt, mBegin, mCap, mSize, slots[20]
    constexpr uintptr_t UCOM_Find      = 0x5D59F0;

    constexpr uint32_t  H_IRigidBody   = 0x4039F0;
    constexpr uint32_t  H_IRBVehicle   = 0x4039E0;

    // Expected vtables (runtime sanity checks - we refuse to touch anything else)
    constexpr uintptr_t VT_PVehicle_IVehicle  = 0x8AA828;
    constexpr uintptr_t VT_RBVehicle_IRigid   = 0x8AC880;
    constexpr uintptr_t VT_Simple_IRigid      = 0x8AC548; // SimpleRigidBody: same slots 8-10,17,23-25,29 (verified)
    constexpr uintptr_t VT_RBVehicle_IRBVeh   = 0x8AC990;

    // AIVehicleRacecar::Update pointer slot; swapping in AIVehicleEmpty's update stops racer AI.
    constexpr uintptr_t RacerAIUpdateSlot = 0x892748;
    constexpr uintptr_t RacerAIUpdate     = 0x42AA80;
    constexpr uintptr_t EmptyAIUpdate     = 0x43CE60;

    // Vehicle creation. The game registers PVehicle::Construct (0x689820) as the "PVehicle" factory
    // (writes it to 0x92C534 at startup). Construct takes a Sim::Param by value and checks:
    //   param.mType == *VehicleParamsType(&out)  (UCrc32 of "VehicleParams", from 0x4040F0)
    //   param.mName == 0xA6B47FAC                ("BASE")
    // then reads VehicleParams at param.mData (+0x10 class, +0x14 model key, +0x18 dir*, +0x1C pos*,
    // +0x20 customization*, +0x24 cache, +0x28 matched perf, +0x2C flags).
    constexpr uintptr_t PVehicleConstruct = 0x689820;
    constexpr uintptr_t VehicleParamsType = 0x4040F0;
    constexpr uint32_t  ParamNameBase     = 0xA6B47FAC;
    constexpr uintptr_t VT_PVehicle_ISimable = 0x8AA940; // slot 2 = Kill (0x6851D0, deferred delete)
}

enum VehicleList { VL_ALL = 0, VL_PLAYERS = 1, VL_AI = 2, VL_AIRACERS = 3, VL_AICOPS = 4, VL_AITRAFFIC = 5 };

// IRigidBody vtable slots (verified)
enum RB {
    RB_GetPosition = 8, RB_GetLinearVelocity = 9, RB_GetAngularVelocity = 10,
    RB_GetOrientation = 17, // quaternion, Vector4
    RB_SetPosition = 23, RB_SetLinearVelocity = 24, RB_SetAngularVelocity = 25,
    RB_SetOrientationQuat = 29,
};
// IVehicle slots
enum IV { IV_GetSimable = 1, IV_IsSpooled = 16, IV_GetVehicleName = 19, IV_GetVehicleKey = 20,
          IV_SetDriverClass = 21, IV_GetDriverClass = 22, IV_IsLoading = 23, IV_GetCustomizations = 50 };
enum DriverClassE { DRV_HUMAN = 0, DRV_TRAFFIC = 1, DRV_COP = 2, DRV_RACER = 3, DRV_NONE = 4, DRV_NIS = 5, DRV_REMOTE = 6 };
// IRBVehicle slots
enum IRBV { IRBV_EnableObjectCollisions = 3 };

namespace {
    bool g_ok = false;
    bool g_coopLinked = true;   // co-op is active right now: connected, and both players are past the prologue

    struct Vec3 { float x, y, z; };
    struct Quat { float x, y, z, w; };

    // ---- raw helpers ----
    template <typename T> T VCall(void* obj, int slot) {
        auto fn = (*(T**)obj)[slot];
        return fn;
    }
    uintptr_t VTable(void* obj) { uintptr_t vt = 0; Mem::Read((uintptr_t)obj, vt); return vt; }

    void* Find(void* iface, uint32_t handle) {
        // iface is a UCOM::IUnknown: [0]=vtable, [4]=Object*
        void* object = nullptr;
        if (!Mem::Read((uintptr_t)iface + 4, object) || !object) return nullptr;
        using Fn = void* (__thiscall*)(void*, uint32_t);
        return ((Fn)Addr::UCOM_Find)(object, handle);
    }

    int ListSize(int list, void*** begin) {
        uintptr_t bucket = Addr::VehicleLists + list * Addr::VehicleBucket;
        void** b = nullptr; uint32_t size = 0;
        if (!Mem::Read(bucket + 4, b) || !Mem::Read(bucket + 0xC, size)) return 0;
        if (!b || size > 64) return 0;
        *begin = b;
        return (int)size;
    }

    void* ListGet(int list, int i) {
        void** b = nullptr;
        int n = ListSize(list, &b);
        if (i < 0 || i >= n) return nullptr;
        void* v = nullptr;
        Mem::Read((uintptr_t)&b[i], v);
        return v;
    }

    bool ListContainsOne(int list, void* v);
    // list == -1: search every vehicle list (used for cars we spawned ourselves)
    bool ListContains(int list, void* v) {
        if (list < 0) {
            for (int l = 0; l < 10; ++l) if (ListContainsOne(l, v)) return true;
            return false;
        }
        return ListContainsOne(list, v);
    }
    bool ListContainsOne(int list, void* v) {
        void** b = nullptr;
        int n = ListSize(list, &b);
        for (int i = 0; i < n; ++i) {
            void* x = nullptr;
            if (Mem::Read((uintptr_t)&b[i], x) && x == v) return true;
        }
        return false;
    }

    bool IsVehicle(void* iv) { return iv && VTable(iv) == Addr::VT_PVehicle_IVehicle; }

    // Accept any IRigidBody whose slots we use point to the exact functions verified in the exe
    // (RBVehicle 0x8AC880 and the remote-player body 0x8AD178 share them).
    bool RigidVtableMatches(uintptr_t vt) {
        static const struct { int slot; uintptr_t fn; } k[] = {
            {RB_GetPosition, 0x671180}, {RB_GetLinearVelocity, 0x671190}, {RB_GetAngularVelocity, 0x6711A0},
            {RB_GetOrientation, 0x671320}, {RB_SetPosition, 0x696FB0}, {RB_SetLinearVelocity, 0x696FD0},
            {RB_SetAngularVelocity, 0x696FF0}, {RB_SetOrientationQuat, 0x696E00} };
        for (auto& e : k) {
            uintptr_t f = 0;
            if (!Mem::Read(vt + e.slot * 4, f) || f != e.fn) return false;
        }
        return true;
    }

    void* RigidBodyOf(void* iv) {
        if (!IsVehicle(iv)) return nullptr;
        void* rb = Find(iv, Addr::H_IRigidBody);
        if (!rb) return nullptr;
        uintptr_t vt = VTable(rb);
        if (vt != Addr::VT_RBVehicle_IRigid && vt != Addr::VT_Simple_IRigid && !RigidVtableMatches(vt)) return nullptr;
        return rb;
    }

    void* RBVehicleOf(void* iv) {
        if (!IsVehicle(iv)) return nullptr;
        void* r = Find(iv, Addr::H_IRBVehicle);
        if (!r) return nullptr;
        // accept any IRBVehicle whose EnableObjectCollisions slot is the verified function (0x6B7CA0)
        uintptr_t f = 0;
        if (VTable(r) != Addr::VT_RBVehicle_IRBVeh && (!Mem::Read(VTable(r) + IRBV_EnableObjectCollisions * 4, f) || f != 0x6B7CA0)) return nullptr;
        return r;
    }

    const float* RB_GetVec(void* rb, int slot) {
        using Fn = const float* (__thiscall*)(void*);
        return VCall<Fn>(rb, slot)(rb);
    }
    void RB_SetVec(void* rb, int slot, const void* v) {
        using Fn = void(__thiscall*)(void*, const void*);
        VCall<Fn>(rb, slot)(rb, v);
    }
    const char* VehName(void* iv) {
        using Fn = const char* (__thiscall*)(void*);
        const char* n = VCall<Fn>(iv, IV_GetVehicleName)(iv);
        return (n && Mem::IsReadable(n, 1)) ? n : "?";
    }
    int DriverClass(void* iv) {
        using Fn = int(__thiscall*)(void*);
        return VCall<Fn>(iv, IV_GetDriverClass)(iv);
    }
    void SetCollisions(void* iv, bool on) {
        void* r = RBVehicleOf(iv);
        if (!r) return;
        using Fn = void(__thiscall*)(void*, bool);
        VCall<Fn>(r, IRBV_EnableObjectCollisions)(r, on);
    }

    // IRBVehicle slot 4 SetInvulnerability(type, seconds) (0x6B7CB0 on RBVehicle: stores +0x44/+0x48).
    // INVULNERABLE_FROM_RESET (2) is the game's post-reset state: the car blinks and ghosts through
    // other cars. On the remote-player body class this slot is an empty stub (harmless).
    void SetInvulnerable(void* iv, int type, float seconds) {
        void* r = RBVehicleOf(iv);
        if (!r) return;
        using Fn = void(__thiscall*)(void*, int, float);
        VCall<Fn>(r, 4)(r, type, seconds);
    }

    void* LocalVehicle() {
        // The players list also holds remote-player cars we spawned (DRIVER_REMOTE): pick the human one.
        void** b = nullptr;
        int n = ListSize(VL_PLAYERS, &b);
        for (int i = 0; i < n; ++i) {
            void* iv = ListGet(VL_PLAYERS, i);
            if (IsVehicle(iv) && DriverClass(iv) == 0 /*DRIVER_HUMAN*/) return iv;
        }
        return nullptr;
    }

    // ---- ghost state ----
    void*  g_ghost = nullptr;       // IVehicle* we are puppeting (an AI racer, or our spawned peer car)
    float  g_push[3] = { 0, 0, 0 };   // velocity we got from being hit by the other player
    DWORD  g_pushFrom = 0;
    DWORD  g_lastHitSent = 0, g_hitT0 = 0;   // our own impact being measured / last one sent
    DWORD  g_noHitUntil = 0;                 // teleport / restart / cutscene: a velocity jump is not an impact
    DWORD  g_testJumpOkUntil = 0;            // the `ram` test command teleports on purpose: don't treat that jump as one
    // lost velocity must point at the car we supposedly hit (horizontal plane)
    bool HitPointsAt(const float* dv, const float* from, const float* to) {
        float dx = to[0] - from[0], dz = to[2] - from[2];
        float dl = sqrtf(dx * dx + dz * dz), vl = sqrtf(dv[0] * dv[0] + dv[2] * dv[2]);
        return dl > 0.01f && vl > 0.01f && (dv[0] * dx + dv[2] * dz) > 0.5f * dl * vl;
    }
    bool   g_ghostStarved = false;
    DWORD  g_noSpawnUntil = 0;      // an event is starting: the race builds the peer's car, don't spawn one  // spawning the peer car failed: traffic mirrors give up budget
    int    g_ghostList = VL_AIRACERS; // which vehicle list g_ghost came from (VL_ALL for our spawned car)
    enum GhostKind { GK_NONE, GK_RACER, GK_SPAWNED, GK_TRAFFIC };
    GhostKind g_ghostKind = GK_NONE;
    uint32_t g_spawnedKey = 0, g_spawnedHash = 0; // which peer car g_ghost was spawned as
    int      g_spawnFails = 0;
    DWORD    g_nextSpawnTry = 0;            // backoff between spawn attempts
    void*    g_replacedRacer = nullptr;            // racer built as the peer's car by the factory hook
    bool     g_racerReplacedThisLoad = false;
    DWORD    g_ghostYieldUntil = 0;            // event start: our copy of their car may be removed for room
    float    g_peerSlot[3] = {};                    // where the race built that racer (its grid slot)
    bool     g_peerSlotValid = false;
    uint32_t g_origRacer[1] = {};
    void*    g_origRacerCustom = nullptr;
    void*    g_origRacerMatched = nullptr;
    uint8_t  g_peerCustomForRace[0x198];
    // Race rosters: each game picks its AI opponents at random, so the game that starts an event sends
    // the opponents it built (model + tuning, in construction order) and the other game builds the same.
    struct RosterEntry { uint32_t key; bool hasCustom; char name[24]; uint8_t custom[0x198]; };
    RosterEntry g_localRoster[8];  int g_localRosterN = 0;     // AI racers built here since the last event
    RosterEntry g_peerRoster[8];   int g_peerRosterN = 0;      // received from the peer
    DWORD    g_peerRosterAt = 0;
    bool     g_usePeerRoster = false;  int g_peerRosterNext = 0;
    bool   g_aiPatched = false;
    bool   g_ghostCollide = false;  // collisions currently enabled on the ghost
    bool   g_separated = false;     // has the pair ever been far enough apart?
    char   g_ghostName[48] = "";
    float  g_jitterMm = 0, g_extrapMs = 0;
    bool   g_protectHoldReset = false;  // set when a new ghost is acquired: start a hold period
    bool   g_protectHolding = false;
    LARGE_INTEGER g_qpf{}, g_lastQpc{};

    // Switch off the racing AI of ONE car (the ones we drive from the network), leaving every other
    // racer alone: give that car's AI object a private copy of its vtable with the update slot
    // (0x892748 in the shared vtable) pointing to an empty function.
    struct AIOverride { void* iv; uint32_t* field; uint32_t origVt; uint32_t copy[0x200]; };
    AIOverride g_aiOv[12];

    bool DisableAI(void* iv) {
        if (!IsVehicle(iv)) return false;
        for (auto& o : g_aiOv) if (o.iv && !ListContains(-1, o.iv)) o.iv = nullptr;   // cars that are gone
        for (auto& o : g_aiOv) if (o.iv == iv) return true;
        void* ai = VCall<void* (__thiscall*)(void*)>(iv, 0xAC / 4)(iv);   // GetAIVehiclePtr
        if (!ai) return false;
        // find the field holding the racer vtable (a pointer a little below the update slot)
        uint32_t* field = nullptr; uint32_t vt = 0;
        for (int off = -0x100; off <= 0x100; off += 4) {
            uint32_t v = 0;
            if (!Mem::Read((uintptr_t)ai + off, v)) continue;
            if (v > Addr::RacerAIUpdateSlot || v + 0x600 < Addr::RacerAIUpdateSlot || (v & 3)) continue;
            uint32_t first = 0, slot = 0;
            if (!Mem::Read(v, first) || first < 0x401000 || first > 0x880000) continue;
            if (!Mem::Read(Addr::RacerAIUpdateSlot, slot) || slot != Addr::RacerAIUpdate) continue;
            if (v > vt) { vt = v; field = (uint32_t*)((uintptr_t)ai + off); }
        }
        if (!field) { LOG("AI: no racer vtable found on %p", iv); return false; }
        for (auto& o : g_aiOv) {
            if (o.iv) continue;
            o.iv = iv; o.field = field; o.origVt = vt;
            Mem::SafeRead((void*)(vt - 4), o.copy, sizeof(o.copy));        // RTTI pointer + entries
            o.copy[1 + (Addr::RacerAIUpdateSlot - vt) / 4] = Addr::EmptyAIUpdate;
            *field = (uint32_t)&o.copy[1];
            LOG("AI: disabled racing AI of %p (vtable %08X)", iv, vt);
            return true;
        }
        return false;
    }
    void RestoreAI(void* iv) {
        for (auto& o : g_aiOv) {
            if (!o.iv || (iv && o.iv != iv)) continue;
            if (Game::GameFlowState() == 6 && ListContains(-1, o.iv)) __try { if (*o.field == (uint32_t)&o.copy[1]) *o.field = o.origVt; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            o.iv = nullptr;
        }
    }
    // A cutscene (NIS: race intro, arrest, story scene) is running: the game animates the cars itself.
    // INIS::mInstance 0x9885C8, state dword +0x2AC (3 loaded, 4/6 pre/post movie, 5 playing, 7 skipped)
    bool NisActive() {
        void* nis = nullptr; int st = 0;
        if (!Mem::Read(0x9885C8, nis) || !nis || !Mem::Read((uintptr_t)nis + 0x2AC, st)) return false;
        return st >= 3 && st <= 6;
    }    void PatchRacerAI(bool disable) {   // the ghost racer only
        if (disable) { if (g_ghost) DisableAI(g_ghost); }
        else if (g_ghost) RestoreAI(g_ghost);
        g_aiPatched = disable;
    }

    Quat Nlerp(Quat a, Quat b, float t) {
        float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (dot < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
        Quat r{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
        float len = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
        if (len < 1e-6f) return b;
        r.x /= len; r.y /= len; r.z /= len; r.w /= len;
        return r;
    }

    float Dist(const float* a, const float* b) {
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return sqrtf(dx * dx + dy * dy + dz * dz);
    }
}

static double SimClockMs();

bool Game::Init() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    uint32_t peOff = 0, ep = 0;
    if (!Mem::Read(base + 0x3C, peOff) || !Mem::Read(base + peOff + 0x28, ep) || base + ep != Addr::EntryPoint) {
        LOG("Game::Init: unexpected speed.exe (entry %08X) - game features disabled", (unsigned)(base + ep));
        return false;
    }
    uintptr_t slot = 0;
    Mem::Read(Addr::RacerAIUpdateSlot, slot);
    if (slot != Addr::RacerAIUpdate) {
        LOG("Game::Init: racer AI slot holds %08X, expected %08X (another mod?) - ghost disabled", (unsigned)slot, (unsigned)Addr::RacerAIUpdate);
        return false;
    }
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_lastQpc);
    g_ok = true;
    LOG("Game::Init: speed.exe v1.3 detected");
    InstallVehicleFactoryHook();
    Net::LocalClockMs = &SimClockMs;
    return true;
}

int Game::GameFlowState() {
    int s = -1;
    if (!Mem::Read(Addr::GameFlowState, s)) return -1;
    return s;
}

static double SimClockMs() {
    float t = 0;
    if (!Mem::Read(0x9885D8, t)) return Net::NowMs();   // Sim::GetTime() reads this float (seconds)
    return (double)t * 1000.0;
}
float Game::SimTimeSec() { float t = 0; Mem::Read(0x9885D8, t); return t; }

static void* RacerInfoOfLocal() {
    void* rs = nullptr;
    if (!Mem::Read(0x91E000, rs) || !rs) return nullptr;
    void* iv = LocalVehicle();
    if (!IsVehicle(iv)) return nullptr;
    void* sim = VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv);
    return sim ? ((void*(__thiscall*)(void*, void*))0x5E8B20)(rs, sim) : nullptr;
}

// Test: hand the local car to the game's racer AI and back (IHumanAI::SetAiControl, the same call the
// game uses after the finish line). In a race it drives the route, in free roam it cruises the roads.
static bool g_apByUs = false;      // the autopilot was switched on by the mod (F5 / AutoPilot), not by the game
static bool g_apUserOff = false;   // the player switched the automatic autopilot off (until the next world/race)
static bool AutopilotIsOn(void* hai) { return VCall<bool(__thiscall*)(void*)>(hai, 3)(hai); }

static const char* ToggleAutopilotSafe() {
    __try {
        void* iv = LocalVehicle();
        void* hai = IsVehicle(iv) ? Find(iv, 0x403770 /*IHumanAI*/) : nullptr;
        if (!hai) return "no human AI on the local car";
        bool on = VCall<bool(__thiscall*)(void*)>(hai, 3)(hai);
        VCall<void(__thiscall*)(void*, bool)>(hai, 4)(hai, !on);
        if (!on) {
            void* vai = VCall<void* (__thiscall*)(void*)>(iv, 0xAC / 4)(iv);
            if (vai) VCall<void(__thiscall*)(void*, int)>(vai, 0x50 / 4)(vai, 1);   // ResetDriveToNav(current lane)
        } else if (void* rb = Find(iv, Addr::H_IRBVehicle)) {
            VCall<void(__thiscall*)(void*, int, float)>(rb, 4)(rb, 3, 1.0f);       // 1 s invulnerability
        }
        LOG("Autopilot %s", on ? "OFF" : "ON");
        g_apUserOff = on; g_apByUs = !on;
        return on ? "autopilot off" : "autopilot ON (AI drives your car)";
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "exception"; }
}
// Nitrous for the autopilot: the racer AI that drives the local car never presses NOS, so while it
// drives we press it on straights. IInput (handle 0x403F90): slot 2 GetControls, slot 8
// SetControlNOS(bool); the controls block starts at IInput+8 (steering +0x0C, gas +0x1C, brake +0x20,
// handbrake +0x24, NOS byte +0x29 - all relative to the IInput pointer). The engine only lets the
// press through while IEngine (handle 0x404020) slot 7 - nitrous left, 0..1 - is above 0.
static void AutopilotNos(void* iv, void* hai) {
    static bool pressing = false;
    static DWORD holdUntil = 0, coolUntil = 0, lastLog = 0;
    void* in = Find(iv, 0x403F90 /*IInput*/);
    if (!in) return;
    auto setNos = [&](bool on) { VCall<void(__thiscall*)(void*, bool)>(in, 8)(in, on); pressing = on; };
    if (!g_cfg.autoPilotNos || !g_apByUs || !hai || !AutopilotIsOn(hai)) { if (pressing) setNos(false); return; }   // never for the game's own AI control (after the finish line)
    const uint8_t* c = (const uint8_t*)in;
    float steer = *(const float*)(c + 0x0C), gas = *(const float*)(c + 0x1C), brake = *(const float*)(c + 0x20);
    float speed = 0;
    if (void* rb = RigidBodyOf(iv)) { const float* v = RB_GetVec(rb, RB_GetLinearVelocity); speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
    DWORD now = GetTickCount();
    // Like a racer would: only in a race, on a real straight (steady for 1.5 s, i.e. out of the corner),
    // with at least half a tank, and not when already near top speed. A burst runs while the straight
    // lasts and stops at 10 % so there's always some left; then the tank refills for the next straight.
    void* eng = Find(iv, 0x404020 /*IEngine*/);
    float tank = eng ? VCall<float(__thiscall*)(void*)>(eng, 7)(eng) : 0.f;
    int mode = 0; void* rs = nullptr;
    if (Mem::Read(0x91E000, rs) && rs) Mem::Read((uintptr_t)rs + 0x1960, mode);
    bool straight = fabsf(steer) < 0.12f && gas > 0.8f && brake < 0.05f && speed > 22.0f;   // ~80 km/h
    static DWORD straightSince = 0;
    if (!straight) straightSince = 0; else if (!straightSince) straightSince = now;
    if (pressing) {
        if (!straight || tank < 0.10f || mode != 1) { setNos(false); coolUntil = now + 2000; }
        else setNos(true);                     // keep it pressed in case the AI cleared the controls
    } else if (mode == 1 && straight && now - straightSince > 1500 && tank >= 0.5f && speed < 72.0f /*~260 km/h*/ && (int)(now - coolUntil) > 0) {
        setNos(true);
        if (now - lastLog > 30000) { lastLog = now; LOG("Autopilot: nitrous on a straight at %.0f km/h (%.0f%% in the tank)", speed * 3.6f, tank * 100.0f); }
    }
    (void)holdUntil;
}
// [Debug] AutoPilot=1: switch the autopilot on whenever we're driving in free roam or a race.
void Game::AutopilotTick() {
    if (g_ok && GameFlowState() == 6 && !NisActive()) {
        __try {
            void* iv = LocalVehicle();
            if (IsVehicle(iv)) AutopilotNos(iv, Find(iv, 0x403770));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!g_ok || !g_cfg.autoPilot) return;
    static int lastFlow = -1, lastMode = -1;
    int flow = GameFlowState(), mode = 0;
    void* rs = nullptr;
    if (Mem::Read(0x91E000, rs) && rs) Mem::Read((uintptr_t)rs + 0x1960, mode);
    if (flow != lastFlow || mode != lastMode) g_apUserOff = false;   // new world or race: auto again
    lastFlow = flow; lastMode = mode;
    static DWORD next = 0;
    if (flow != 6 || g_apUserOff || NisActive() || GetTickCount() < next) return;
    next = GetTickCount() + 1000;
    __try {
        void* iv = LocalVehicle();
        void* hai = IsVehicle(iv) ? Find(iv, 0x403770) : nullptr;
        if (!hai || AutopilotIsOn(hai)) return;
        ToggleAutopilotSafe();
        g_apUserOff = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

std::string Game::ToggleAutopilot() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    return ToggleAutopilotSafe();
}

// Visual state of a car: its IInput controls (handle 0x403F90; controls block at IInput+8: steering
// +0x0C, gas +0x1C, brake +0x20, handbrake +0x24, NOS byte +0x29 - relative to the IInput pointer;
// setters slot 5 steering, 6 gas, 7 brake, 8 NOS(bool), 9 handbrake) and its light bits (IVehicle+0x9C).
static uint8_t Unit8(float v) { return (uint8_t)(v <= 0.f ? 0 : v >= 1.f ? 255 : (int)(v * 255.f + 0.5f)); }
static void ReadCarControls(void* iv, CarState& out) {
    const uint8_t* in = (const uint8_t*)Find(iv, 0x403F90 /*IInput*/);
    if (in) {
        out.steer = *(const float*)(in + 0x0C);
        if (!(fabsf(out.steer) <= 1.f)) out.steer = 0.f;
        out.gas = Unit8(*(const float*)(in + 0x1C));
        out.brake = Unit8(*(const float*)(in + 0x20));
        out.handbrake = Unit8(*(const float*)(in + 0x24));
        out.nos = in[0x29] ? 1 : 0;
    }
    out.glare = *(const uint32_t*)((const uint8_t*)iv + 0x9C);
    // damage: IDamageable (handle 0x405000) slot 8 GetZoneDamage(out) - 10 zones x 3 bits, the same
    // value it keeps at IDamageable+0x4C; broken lights: IDamageableVehicle (0x668440) +0x5C bit mask
    if (void* dmg = Find(iv, 0x405000)) { uint32_t z = 0; VCall<void*(__thiscall*)(void*, uint32_t*)>(dmg, 8)(dmg, &z); out.damageZones = z; }
    if (void* dv = Find(iv, 0x668440)) out.brokenLights = *(const uint32_t*)((const uint8_t*)dv + 0x5C);
}
// Show the other player's controls on their car here: front-wheel angle (steering), nitrous flames (NOS)
// and the lights (brake / head / reverse / signals from the glare bits). The motion itself is still
// hard-set from the network every frame.
// Dents / broken parts and lights of the other player's car (written only when they change).
static void ApplyCarDamage(void* iv, const CarState& s) {
    if (!g_cfg.syncDamage) return;
    if (uint8_t* dmg = (uint8_t*)Find(iv, 0x405000)) {
        uint32_t& z = *(uint32_t*)(dmg + 0x4C);
        if (z != (s.damageZones & 0x3FFFFFFF)) {
            z = s.damageZones & 0x3FFFFFFF;
            static uint32_t logged = 0xFFFFFFFF;                      // the game may reset it: log only new values
            if (z != logged) { logged = z; LOG("Ghost: damage zones now %08X", z); }
        }
    }
    if (uint8_t* dv = (uint8_t*)Find(iv, 0x668440)) {
        uint32_t& l = *(uint32_t*)(dv + 0x5C);
        if (l != s.brokenLights) { l = s.brokenLights; LOG("Ghost: broken lights now %08X", l); }
    }
}
static void ApplyCarControls(void* iv, const CarState& s) {
    void* in = Find(iv, 0x403F90 /*IInput*/);
    if (in) {
        // gas / brake / handbrake are not fed in: they act on the physics and fought the hard-set motion
        // (50-80 mm jitter spikes on a car driving fast). The brake lights come from the glare bits.
        float steer = s.steer < -1.f ? -1.f : s.steer > 1.f ? 1.f : s.steer;
        VCall<void(__thiscall*)(void*, float)>(in, 5)(in, steer);
        VCall<void(__thiscall*)(void*, bool)>(in, 8)(in, s.nos != 0);
    }
    *(uint32_t*)((uint8_t*)iv + 0x9C) = s.glare;
}
// While the remote car is held (cutscene, spawn protection) it must not keep the last steering / NOS.
static void ClearCarControls(void* iv) {
    void* in = Find(iv, 0x403F90 /*IInput*/);
    if (!in) return;
    VCall<void(__thiscall*)(void*, float)>(in, 5)(in, 0.f);
    VCall<void(__thiscall*)(void*, bool)>(in, 8)(in, false);
}

bool Game::ReadLocalCar(CarState& out) {
    if (!g_ok || GameFlowState() != 6) return false;
    __try {
        void* iv = LocalVehicle();
        void* rb = RigidBodyOf(iv);
        if (!rb) return false;
        const float* p = RB_GetVec(rb, RB_GetPosition);
        const float* q = RB_GetVec(rb, RB_GetOrientation);
        const float* v = RB_GetVec(rb, RB_GetLinearVelocity);
        const float* w = RB_GetVec(rb, RB_GetAngularVelocity);
        memcpy(out.pos, p, 12);
        memcpy(out.rot, q, 16);
        memcpy(out.linVel, v, 12);
        memcpy(out.angVel, w, 12);
        out.speedMps = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        Mem::Read(0x9885D8, out.simTime);                 // when (in simulation time) this state is from
        // race progress of my racer (GRacerInfo: +0x18 pct race, +0x21 finished, +0x24 pct lap,
        // +0x28 laps, +0x2C checkpoints this lap), looked up with GRaceStatus::GetRacerInfo(ISimable*)
        ReadCarControls(iv, out);
        out.inRace = 0;
        void* rs = nullptr;
        int mode = 0;
        if (Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode == 1) {
            void* sim = VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv);
            void* ri = sim ? ((void*(__thiscall*)(void*, void*))0x5E8B20)(rs, sim) : nullptr;
            if (ri) {
                int laps = 0, cps = 0; uint8_t fin = 0;
                Mem::Read((uintptr_t)ri + 0x18, out.racePct);
                Mem::Read((uintptr_t)ri + 0x24, out.lapPct);
                Mem::Read((uintptr_t)ri + 0x28, laps);
                Mem::Read((uintptr_t)ri + 0x2C, cps);
                Mem::Read((uintptr_t)ri + 0x21, fin);
                auto clampf = [](float v) { return (v == v && v > 0.f) ? (v < 100.f ? v : 100.f) : 0.f; };   // the receiver rejects values outside 0..100
                out.racePct = clampf(out.racePct); out.lapPct = clampf(out.lapPct);
                laps = laps < 0 ? 0 : laps > 200 ? 200 : laps; cps = cps < 0 ? 0 : cps > 2000 ? 2000 : cps;
                out.laps = (int16_t)laps; out.checkpoints = (int16_t)cps; out.finished = fin ? Game::LocalFinishRank(ri) : 0;   // our rank at the finish
                { static uint8_t lastFin = 0; if (out.finished && !lastFin) LOG("Race: we finished (rank %d)", (int)out.finished); lastFin = out.finished; }
                // our own name in the standings: the co-op player name (both games may share one
                // save profile, so the profile name would be the same on both)
                static char myName[32];
                if (!g_cfg.playerName.empty()) {
                    NetUtil::CopyHudName(myName, g_cfg.playerName.c_str());       // HUD slots are 16 bytes
                    const char* nm = myName; Mem::Write((uintptr_t)ri + 0x08, nm);
                }
                out.inRace = 1;
            }
        }
        out.pursuitHeat = Game::LocalPursuitHeat();
        out.nisHash = Game::NisSceneHash();
        out.nisVote = Game::NisVoteHash();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("ReadLocalCar: exception");
        return false;
    }
}

// ---------------------------------------------------------------------------
// Spawning our own vehicles
// ---------------------------------------------------------------------------
namespace {
    struct SimParam { uint32_t mType, mName; void* mData; uint32_t pad; };
    struct VehicleParams {
        SimParam base;
        int32_t  carClass;
        uint32_t carType;
        const float* initialVec;
        const float* initialPos;
        void*    customization;
        void*    vehicleCache;
        void*    matched;
        uint32_t flags;
    };
    static_assert(sizeof(VehicleParams) == 0x30, "VehicleParams layout");

    // ---- our IVehicleCache ----
    // PVehicle::Construct runs a car-memory budget check (0x687710). A new car model needs room in the
    // CarLoader pool; when over budget, the game asks the IVehicleCache passed in VehicleParams+0x24 to
    // agree to evicting other cars (traffic/cops). With no cache it simply fails. Interface (UCOM::IUnknown
    // layout: vtable, mCOMObject): [1] GetCacheName, [2] OnQueryVehicleCache(IVehicle*, IVehicleCache*)
    // -> 0 VCR_WANT / 1 VCR_DONTCARE, [3] OnRemovedVehicleCache(IVehicle*).
    // The cache must also be in the game's list of caches (0x425300 on cache+8; array 0x92FDA4): the
    // budget check (0x687710) asks only listed caches before it DELETES a vehicle to make room for a new
    // traffic car / cop, and builds the new one in the same memory. Unlisted, the other player's car was
    // deleted and our pointer then drove a cop or traffic car (seen as "his car turned into a taxi").
    void* g_keepCars[64] = {};                // every car we spawned (the other player's car, mirrors)
    uint32_t g_keepWid[64] = {};              // their world ids: the same address may later hold another car
    // a spot behind and a little to the left of a car, facing the same way (where a partner waits on a grid)
    bool BehindLeftOf(void* rb, float out[3]) {
        __try {
            float f[3], r[3];
            VCall<void(__thiscall*)(void*, float*)>(rb, 13 /*GetForwardVector*/)(rb, f);
            VCall<void(__thiscall*)(void*, float*)>(rb, 14 /*GetRightVector*/)(rb, r);
            const float* p = RB_GetVec(rb, RB_GetPosition);
            for (int i = 0; i < 3; ++i) out[i] = p[i] - f[i] * 7.0f - r[i] * 2.5f;
            out[1] += 0.3f;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    uint32_t WorldIdOf(void* iv) {            // ISimable slot 28 (0x686AF0): unique per car, never reused
        __try {
            void* sim = VCall<void* (__thiscall*)(void*)>(iv, 1 /*IV_GetSimable*/)(iv);
            return sim ? VCall<uint32_t(__thiscall*)(void*)>(sim, 28)(sim) : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    // false if this address now holds a different car than the one we spawned there
    bool StillOurCar(void* iv) {
        for (int i = 0; i < 64; ++i) if (g_keepCars[i] == iv) return !g_keepWid[i] || WorldIdOf(iv) == g_keepWid[i];
        return true;
    }
    void* g_removedCars[16] = {}; int g_removedNext = 0;   // ours the game deleted: the address may hold another car now
    void (*g_onCarRemoved)(void*) = nullptr;
    void (*g_onMirrorRemoved)(void*) = nullptr;     // traffic.inl: drop any mirror that used this car

    bool IsKeptCar(void* iv) { for (void* k : g_keepCars) if (k && k == iv) return true; return false; }
    void KeepCar(void* iv) {
        for (void*& r : g_removedCars) if (r == iv) r = nullptr;     // this address is ours again
        for (int i = 0; i < 64; ++i) if (!g_keepCars[i]) { g_keepCars[i] = iv; g_keepWid[i] = WorldIdOf(iv); return; }
    }
    void ForgetCar(void* iv) { for (void*& k : g_keepCars) if (k == iv) k = nullptr; }
    bool WasRemovedByGame(void* iv) { if (!iv) return false; for (void* r : g_removedCars) if (r == iv) return true; return false; }

    // __fastcall(ecx = this, edx = unused, stack args...) matches MSVC __thiscall with callee cleanup.
    void        __fastcall Cache_Dtor(void*, void*, int) {}
    const char* __fastcall Cache_GetName(void*, void*) { return "MWTogetherRemotePlayers"; }
    // keep the other player's car (and the test clone); mirror cars may go to make room - they come back
    int         __fastcall Cache_OnQuery(void*, void*, void* iv, void* /*asking*/);
    void        __fastcall Cache_OnRemoved(void*, void*, void* iv) {
        if (IsKeptCar(iv)) {
            LOG("Vehicle cache: game removed our car %p", iv);
            ForgetCar(iv);
            g_removedCars[g_removedNext] = iv; g_removedNext = (g_removedNext + 1) % 16;
            if (g_onMirrorRemoved) g_onMirrorRemoved(iv);
            if (g_onCarRemoved) g_onCarRemoved(iv);
        }
    }
    void* g_cacheVtbl[4] = { (void*)&Cache_Dtor, (void*)&Cache_GetName, (void*)&Cache_OnQuery, (void*)&Cache_OnRemoved };
    struct VCache { void** vt; void* com; void* node; };
    VCache g_cache = { g_cacheVtbl, (void*)0x4D575443 /* unique tag, compared only */, nullptr };
    bool g_cacheListed = false;
    void ListOurCache() {
        if (g_cacheListed) return;
        __try { ((void(__thiscall*)(void*))0x425300)((char*)&g_cache + 8); g_cacheListed = true; LOG("Vehicle cache: registered with the game"); }
        __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Vehicle cache: registering failed"); }
    }

    int __fastcall Cache_OnQuery(void*, void*, void* iv, void* /*asking*/) {
        // while an event we join builds its racers, the game may take our copy of their car for room
        bool yield = g_ghostYieldUntil && (int)(GetTickCount() - g_ghostYieldUntil) < 0;
        return (iv && iv == g_ghost && !yield) ? 0 /*want*/ : 1 /*don't care*/;
    }

    uint32_t VehicleKey(void* iv) {
        using Fn = uint32_t(__thiscall*)(void*);
        return VCall<Fn>(iv, IV_GetVehicleKey)(iv);
    }
    const void* Customizations(void* iv) {
        using Fn = const void* (__thiscall*)(void*);
        return VCall<Fn>(iv, IV_GetCustomizations)(iv);
    }

    void* g_killed[32] = {}; int g_killedNext = 0;   // cars we destroyed (see KillVehicle)
    void* SpawnVehicle(uint32_t carKey, const void* custom, const float pos[3], const float dir[3], int driverClass) {
        uint32_t typeHash = 0;
        ((uint32_t* (__cdecl*)(uint32_t*))Addr::VehicleParamsType)(&typeHash);
        static uint8_t customCopy[0x198];           // the game copies it during construction
        if (custom) memcpy(customCopy, custom, sizeof(customCopy));
        VehicleParams vp{};
        vp.base.mType = typeHash;
        vp.base.mName = Addr::ParamNameBase;
        vp.base.mData = &vp;
        vp.carClass = driverClass;
        vp.carType = carKey;
        vp.initialVec = dir;
        vp.initialPos = pos;
        vp.customization = custom ? customCopy : nullptr;
        vp.vehicleCache = &g_cache;   // lets the budget check evict traffic/cops to make room for this model
        using Fn = void* (__cdecl*)(SimParam);
        void* simable = ((Fn)Addr::PVehicleConstruct)(vp.base);
        if (!simable) return nullptr;
        // find its IVehicle interface
        void* iv = nullptr;
        void* object = nullptr;
        if (Mem::Read((uintptr_t)simable + 4, object) && object) {
            using FindFn = void* (__thiscall*)(void*, uint32_t);
            iv = ((FindFn)Addr::UCOM_Find)(object, 0x4040E0 /*IVehicle*/);
        }
        LOG("SpawnVehicle key %08X class %d -> simable %p ivehicle %p", carKey, driverClass, simable, iv);
        if (iv) { for (void*& k : g_killed) if (k == iv) k = nullptr; KeepCar(iv); }
        return IsVehicle(iv) ? iv : nullptr;
    }

    // a car we already destroyed must never be destroyed again (seen at a race start: the same car killed
    // twice in one frame, then the game called into the freed object - "pure virtual function call")
    bool AlreadyKilled(void* iv) { for (void* k : g_killed) if (k == iv) return true; return false; }
    void KillVehicle(void* iv) {
        if (!iv) return;
        if (AlreadyKilled(iv)) { LOG("KillVehicle %p skipped - already destroyed", iv); return; }
        if (!StillOurCar(iv)) { LOG("KillVehicle %p skipped - the game built another car there", iv); ForgetCar(iv); return; }
        ForgetCar(iv);
        if (!IsVehicle(iv)) return;
        using GetSim = void* (__thiscall*)(void*);
        void* sim = VCall<GetSim>(iv, IV_GetSimable)(iv);
        if (!sim || VTable(sim) != Addr::VT_PVehicle_ISimable) { LOG("KillVehicle: unexpected ISimable %p", sim); return; }
        using KillFn = void(__thiscall*)(void*);
        VCall<KillFn>(sim, 2)(sim);
        g_killed[g_killedNext] = iv; g_killedNext = (g_killedNext + 1) % 32;
        LOG("KillVehicle %p", iv);
    }

    void* g_clone = nullptr;
}

// ---------------------------------------------------------------------------
// Races: the game builds every car through the "PVehicle" factory pointer (0x92C534). While a race
// loads, build the first AI racer as the peer's car (model + tuning) so they race in their own car
// and count in positions / results. (Same technique as MostWantedTimeTrialGhosts.)
// ---------------------------------------------------------------------------
// A Blacklist rival (boss) race has a single opponent: the rival. There the other player must not
// take that racer's place, so their car is spawned next to the race instead (not in the standings).
// The event being started is known either from the race status or from the invitation we act on.
static uint32_t g_startingEventHash = 0;
static bool BossRaceStarting() {
    __try {
        void* rs = nullptr; void* parms = nullptr; void* db = nullptr;
        if (Mem::Read(0x91E000, rs) && rs) Mem::Read((uintptr_t)rs + 0x1968, parms);
        if (!parms && g_startingEventHash && Mem::Read(0x91E004, db) && db)
            parms = ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, g_startingEventHash);
        return parms && ((bool(__thiscall*)(void*))0x5FBD90)(parms);     // GRaceParameters::IsBossRace
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void* __cdecl hk_VehicleConstruct(SimParam p) {
    using Fn = void* (__cdecl*)(SimParam);
    VehicleParams* vp = (VehicleParams*)p.mData;
    bool replaced = false;
    if (vp && vp->carClass == DRV_RACER && !g_racerReplacedThisLoad && g_cfg.ghostEnabled && Net::Stats().connected && g_coopLinked && !BossRaceStarting()) {
        if (const CarInfo* pc = Net::PeerCar()) {
            g_origRacer[0] = vp->carType; g_origRacerCustom = vp->customization; g_origRacerMatched = vp->matched;
            vp->carType = pc->carKey;
            if (pc->hasCustom) { memcpy(g_peerCustomForRace, pc->custom, sizeof(g_peerCustomForRace)); vp->customization = g_peerCustomForRace; }
            else vp->customization = nullptr;
            vp->matched = nullptr;
            replaced = true;
            g_racerReplacedThisLoad = true;
        }
    }

    // the other AI racers: build the same opponents as the game that started the event
    bool fromRoster = false;
    uint32_t rKey = 0; void* rCustom = nullptr; void* rMatched = nullptr;
    if (vp && vp->carClass == DRV_RACER && !replaced && g_coopLinked && g_usePeerRoster && g_peerRosterNext < g_peerRosterN) {
        RosterEntry& r = g_peerRoster[g_peerRosterNext++];
        rKey = vp->carType; rCustom = vp->customization; rMatched = vp->matched;
        vp->carType = r.key;
        vp->customization = r.hasCustom ? (void*)r.custom : nullptr;
        vp->matched = nullptr;
        fromRoster = true;
    }

    void* simable = ((Fn)Addr::PVehicleConstruct)(p);
    if (fromRoster) {
        if (simable) LOG("Race: opponent %d built as the peer's opponent (key %08X)", g_peerRosterNext, vp->carType);
        else {
            LOG("Race: peer's opponent model %08X not loaded - keeping our own", vp->carType);
            vp->carType = rKey; vp->customization = rCustom; vp->matched = rMatched;
            simable = ((Fn)Addr::PVehicleConstruct)(p);
        }
    }
    // remember what we built, to send it when this game starts the event
    if (simable && vp && vp->carClass == DRV_RACER && !replaced && g_localRosterN < 8) {
        RosterEntry& r = g_localRoster[g_localRosterN++];
        r.key = vp->carType;
        r.hasCustom = vp->customization != nullptr;
        if (r.hasCustom) Mem::SafeRead(vp->customization, r.custom, sizeof(r.custom));
        r.name[0] = 0;
    }
    if (replaced && !simable) {
        // Their model isn't loaded here (seen when they started the event far away from us). The racer
        // is built as the game wanted it and still becomes theirs - their name, their progress, driven
        // by their position - so they are in the standings, only in this AI car's body. (Before, no
        // racer was theirs: they were missing from the standings and their car was spawned outside.)
        uint32_t want = vp->carType;
        vp->carType = g_origRacer[0];
        vp->customization = g_origRacerCustom;
        vp->matched = g_origRacerMatched;
        simable = ((Fn)Addr::PVehicleConstruct)(p);
        if (simable) LOG("Race: the other player's car model %08X isn't loaded here - their racer keeps this AI car's model (%08X)", want, vp->carType);
        else { replaced = false; g_racerReplacedThisLoad = false; LOG("Race: racer for the other player could not be built (model %08X)", want); }
    }
    if (replaced && simable) {
        void* object = nullptr;
        if (Mem::Read((uintptr_t)simable + 4, object) && object) {
            using FindFn = void* (__thiscall*)(void*, uint32_t);
            g_replacedRacer = ((FindFn)Addr::UCOM_Find)(object, 0x4040E0);
        }
        LOG("Race: AI racer built as peer car (key %08X) -> %p", vp->carType, g_replacedRacer);
        g_peerSlotValid = vp->initialPos && Mem::SafeRead(vp->initialPos, g_peerSlot, sizeof(g_peerSlot)) &&
                          NetUtil::FiniteWithin(g_peerSlot, 3, NetUtil::MAX_POS);
        if (g_peerSlotValid) LOG("Race: the other player's grid slot %.1f %.1f %.1f", g_peerSlot[0], g_peerSlot[1], g_peerSlot[2]);
    }
    return simable;
}

bool Game::InstallVehicleFactoryHook() {
    ListOurCache();
    g_onCarRemoved = [](void* iv) {
        if (iv == g_ghost) { g_ghost = nullptr; g_ghostKind = GK_NONE; g_spawnedKey = g_spawnedHash = 0; }
    };
    uintptr_t cur = 0;
    if (!Mem::Read(0x92C534, cur) || cur != Addr::PVehicleConstruct) {
        LOG("Vehicle factory slot holds %08X, expected %08X - race car replacement disabled", (unsigned)cur, (unsigned)Addr::PVehicleConstruct);
        return false;
    }
    Mem::PatchPointer((void*)0x92C534, (void*)&hk_VehicleConstruct);
    InstallTrafficHooks();
    InstallEventSync();
    InstallRestartSync();
    InstallStreamHook();
    InstallNisHook();
    InstallStoryHooks();
    InstallSaveSync();
    Net::OnHit = [](const float* dv) {
        if (!g_coopLinked) return;
        __try {
            void* lrb = RigidBodyOf(LocalVehicle());
            if (!lrb || Game::GameFlowState() != 6 || !g_ghostCollide || !g_separated || g_protectHolding) return;
            void* grb = RigidBodyOf(g_ghost);                       // only when their car really is next to us
            if (!grb || Dist(RB_GetVec(grb, RB_GetPosition), RB_GetVec(lrb, RB_GetPosition)) > 10.f) return;
            float mag = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
            float k = 0.6f * (mag > 45.f ? 45.f / mag : 1.f);          // part of the impact, capped
            // A standing player car brakes itself, so one velocity write is gone within a frame or two:
            // remember the push and keep the car moving along it for 0.4 s (fading out), see ApplyPush.
            g_push[0] = dv[0] * k; g_push[1] = 0; g_push[2] = dv[2] * k;
            g_pushFrom = GetTickCount();
            g_hitT0 = 0; g_lastHitSent = g_pushFrom;      // this is THEIR impact: don't report the push back as ours
            LOG("Collision: hit by the other player, pushed by %.0f km/h", mag * k * 3.6f);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    };
    InstallCoopWin();
    InstallCatchUp();
    InstallNoSlowMotion();
    InstallWeatherSync();
    InstallMinimapHook();
    InstallWorldMapHook();
    InstallPursuitHooks();
    LOG("Hooked vehicle factory (races: AI racer -> peer car)");
    return true;
}

void Game::OnFlow(int flow) {
    if (flow != 6) {
        for (auto& o : g_aiOv) o.iv = nullptr;   // world unloading: the cars (and their AI) are gone
        for (void*& k : g_keepCars) k = nullptr;
        for (void*& r : g_removedCars) r = nullptr;
    }
    static int last = -1;
    if (flow != last) {
        if (flow == 4) { g_racerReplacedThisLoad = false; g_replacedRacer = nullptr; g_spawnFails = 0; } // new world loading
        last = flow;
    }
}

bool Game::ReadLocalCarInfo(CarInfo& out) {
    if (!g_ok || GameFlowState() != 6) return false;
    __try {
        void* iv = LocalVehicle();
        if (!iv) return false;
        memset(&out, 0, sizeof(out));
        // Send the MODEL key (hash of the car's model name, e.g. "eclipsegt"), not GetVehicleKey():
        // career cars can carry a per-save key (seen: 401337DE for a career eclipsegt) that doesn't
        // exist in the other player's game, so Construct fails there.
        out.carKey = VehicleKey(iv);
        const char* nm = VehName(iv);
        if (nm && nm[0] && nm[0] != '?') {
            using HashFn = uint32_t(__cdecl*)(const char*);
            uint32_t modelKey = ((HashFn)0x4519D0)(nm);   // Attrib::StringHash32
            if (modelKey) out.carKey = modelKey;
        }
        static uint32_t lastLoggedKey = 0;
        if (out.carKey != lastLoggedKey) {
            lastLoggedKey = out.carKey;
            LOG("Local car name: %s (key %08X, driver class %d)", VehName(iv), out.carKey, DriverClass(iv));
        }
        if (const void* c = Customizations(iv)) {
            memcpy(out.custom, c, CUSTOM_RECORD_SIZE);
            out.hasCustom = 1;
        }
        uint32_t h = 2166136261u; // FNV-1a
        for (int i = 0; i < CUSTOM_RECORD_SIZE; ++i) { h ^= out.custom[i]; h *= 16777619u; }
        out.customHash = h ^ out.carKey;
        return out.carKey != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

#include "traffic.inl"
#include "event.inl"
#include "minimap.inl"
#include "worldmap.inl"
#include "pursuit.inl"
#include "stream.inl"
static bool SaveSyncHoldsBoot();
static char g_dataDir[MAX_PATH] = "";
void Game::SetDataDir(const char* dir) { strncpy_s(g_dataDir, dir, _TRUNCATE); }
#include "boot.inl"
#include "nis.inl"
#include "story.inl"
#include "save.inl"
#include "coopwin.inl"
#include "weather.inl"

void Game::ReleaseGhost() {
    g_streamPeer = false;
    if (g_ghost) {
        __try {
            // Only touch the car if the game still owns it (it may have been destroyed after a race).
            if (GameFlowState() == 6 && ListContains(g_ghostList, g_ghost)) {
                if (g_ghostKind == GK_SPAWNED) KillVehicle(g_ghost); // our own car: remove it
                else SetCollisions(g_ghost, true);                 // borrowed car: give it back
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try {
            if (g_ghostKind == GK_RACER) RestoreAI(g_ghost);          // give the racer its AI back
            ReleaseRetarget(PursuitOf(LocalVehicle()));               // no cop may keep chasing a dead car
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_ghostKind = GK_NONE;
        g_spawnedKey = g_spawnedHash = 0;
        LOG("Ghost released");
        g_ghost = nullptr;
        g_ghostName[0] = 0;
    }
    PatchRacerAI(false);
    g_ghostCollide = false;
    g_separated = false;
}

// Race standings: the other player's nickname (GRacerInfo::mName, +0x08) and progress on their racer.
// Every frame from the moment their racer is ours - also during the intro cutscene and while spawn
// protection holds their car - so the standings show them right from the countdown (the progress that
// decides the order is put in before the game sorts, see hk_RacerUpdate in coopwin.inl).
static void PeerRacerInfoTick() {
    static char peerName[32];
    const std::string& pn = Net::Stats().peerName;
    if (pn.empty() || !g_ghost) return;
    NetUtil::CopyHudName(peerName, pn.c_str());       // HUD slots are 16 bytes
    void* rs = nullptr;
    Mem::Read(0x91E000, rs);
    void* sim = VCall<void* (__thiscall*)(void*)>(g_ghost, IV_GetSimable)(g_ghost);
    if (!rs || !sim) return;
    void* ri = ((void*(__thiscall*)(void*, void*))0x5E8B20)(rs, sim);  // GRaceStatus::GetRacerInfo(ISimable*)
    static void* loggedRi = nullptr;
    if (ri != loggedRi) { loggedRi = ri; LOG("Race: peer racer info %p (ghost %p, kind %d) name %s", ri, g_ghost, (int)g_ghostKind, peerName); }
    if (!ri) return;
    const char* nm = peerName; Mem::Write((uintptr_t)ri + 0x08, nm);
    // (+0x21 is not written: it isn't reliably "finished" and ranked a parked peer 1st)
    float used = 0;
    if (!WritePeerProgress(ri, &used)) return;
    static DWORD nextLog = 0;
    if (GetTickCount() >= nextLog) {
        nextLog = GetTickCount() + 5000;
        const CarState& ls = Net::Latest().s;
        int rank = 0; Mem::Read((uintptr_t)ri + 0x10, rank);
        void* mri = RacerInfoOfLocal();
        int myRank = 0; float myPct = 0;
        if (mri) { Mem::Read((uintptr_t)mri + 0x10, myRank); Mem::Read((uintptr_t)mri + 0x18, myPct); }
        LOG("Race: peer pct %.3f (sent %.3f) lap %.3f laps %d cps %d fin %d -> rank %d | me pct %.3f rank %d",
            used, ls.racePct, ls.lapPct, (int)ls.laps, (int)ls.checkpoints, (int)ls.finished, rank, myPct, myRank);
    }
}

// Client, race start: both games put their own player on the player's grid slot, so on each screen the
// other player's car would stand inside ours (spawn protection then held it still until the two were
// 10 m apart: their car stood on the grid for the first seconds of the race). The client moves its car
// to the slot its game built the host's racer on - the same slot the host's game built the client's
// racer on - so both screens agree and the cars stand apart from the countdown on.
static void TakePeerSlotTick(const Net::Snapshot& latest, void* ghostRb) {
    static void* slotOf = nullptr; static uint8_t lastRestart = 0; static DWORD windowUntil = 0;
    if (g_cfg.isHost || !g_cfg.separateGridSlots || g_ghostKind != GK_RACER || g_ghost != g_replacedRacer || !g_peerSlotValid) return;
    uint8_t restart = 0; Mem::Read(0x91CF28, restart);
    // a new race (new racer) or a restart (the game puts everyone back on the grid) opens the window
    if (slotOf != g_replacedRacer || (restart && !lastRestart)) { slotOf = g_replacedRacer; windowUntil = GetTickCount() + 60000; }
    lastRestart = restart;
    if (!windowUntil || (int)(GetTickCount() - windowUntil) > 0) return;
    void* rs = nullptr; int mode = 0;
    if (!Mem::Read(0x91E000, rs) || !rs || !Mem::Read((uintptr_t)rs + 0x1960, mode) || mode != 1) return;
    if (!latest.valid || !latest.s.hasCar || !latest.s.inRace || latest.s.eventHash != Game::CurrentEventHash()) return;
    void* lrb = RigidBodyOf(LocalVehicle());
    if (!lrb) return;
    const float* lp = RB_GetVec(lrb, RB_GetPosition);
    const float* lv = RB_GetVec(lrb, RB_GetLinearVelocity);
    float mySpeed = sqrtf(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]);
    if (!NetUtil::ShouldTakePeerSlot(lp, mySpeed, latest.s.pos, latest.s.speedMps, g_peerSlot)) return;
    float from[3] = { lp[0], lp[1], lp[2] };
    float to[3] = { g_peerSlot[0], g_peerSlot[1] + 0.2f, g_peerSlot[2] }, zero[3] = { 0, 0, 0 };
    RB_SetVec(lrb, RB_SetPosition, to);                 // grid slots face the same way: keep our heading
    RB_SetVec(lrb, RB_SetLinearVelocity, zero); RB_SetVec(lrb, RB_SetAngularVelocity, zero);
    if (ghostRb) {                                      // their car onto the slot we left (where they are)
        RB_SetVec(ghostRb, RB_SetOrientationQuat, latest.s.rot);
        RB_SetVec(ghostRb, RB_SetPosition, latest.s.pos);
        RB_SetVec(ghostRb, RB_SetLinearVelocity, zero); RB_SetVec(ghostRb, RB_SetAngularVelocity, zero);
    }
    windowUntil = 0;
    LOG("Start: we stood on the host's grid slot - moved to the slot of the host's racer (%.1f m)", Dist(from, to));
}

static void UpdateGhostImpl(const Net::Snapshot& latest, const Net::Snapshot& prev, bool enabled, char* out, size_t outLen) {
#define RET(msg) do { strncpy_s(out, outLen, msg, _TRUNCATE); return; } while (0)
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - g_lastQpc.QuadPart) / (float)g_qpf.QuadPart;
    g_lastQpc = now;
    if (dt > 0.1f) dt = 0.1f;

    if (!enabled) { Game::ReleaseGhost(); RET("disabled (F9)"); }
    if (Game::GameFlowState() != 6) { g_ghost = nullptr; Game::ReleaseGhost(); for (auto& o : g_aiOv) o.iv = nullptr; RET("waiting (not in game)"); }
    // The game may delete our copy of their car (to build a race car) and build another car in its memory:
    // then the pointer is someone else's car (seen: the rival's GTI). Let go of it, never touch it again.
    if (g_ghost && g_ghostKind == GK_SPAWNED && !StillOurCar(g_ghost)) {
        LOG("Ghost: the game replaced our copy of their car with another car - letting go of it");
        ForgetCar(g_ghost);
        g_ghost = nullptr; g_ghostKind = GK_NONE; g_spawnedKey = g_spawnedHash = 0;
    }
    // Drop the ghost if the game removed that car.
    if (g_ghost && !ListContains(g_ghostList, g_ghost)) {
        LOG("Ghost car left its vehicle list (despawned)");
        g_ghost = nullptr;
        Game::ReleaseGhost();
    }
    {   // their pause menu / a short gap reports "no car" while they're still in the world: keep the car
        // standing where it was for a while instead of removing it and spawning it again (each new spawn
        // can come out wrong, and it reappeared next to us)
        static DWORD noCarSince = 0;
        bool noCar = !latest.valid || !latest.s.hasCar;
        if (!noCar) noCarSince = 0;
        else {
            if (!noCarSince) noCarSince = GetTickCount();
            bool stillInWorld = latest.valid && latest.s.gameFlowState == 6;
            if (g_ghost && stillInWorld && GetTickCount() - noCarSince < 120000) {
                if (void* rb = RigidBodyOf(g_ghost)) { float z[3] = { 0, 0, 0 }; RB_SetVec(rb, RB_SetLinearVelocity, z); RB_SetVec(rb, RB_SetAngularVelocity, z); }
                RET("paused there - car kept in place");
            }
            Game::ReleaseGhost(); RET("waiting (peer not driving)");
        }
    }



    // Using a borrowed traffic car but a spawn retry is due: give it back and try spawning the real car.
    if (g_ghost && g_ghostKind == GK_TRAFFIC && Net::PeerCar() && GetTickCount() >= g_nextSpawnTry) Game::ReleaseGhost();

    // An event started (from free roam): the race built a racer as the peer's car. Use that one and
    // drop our free-roam copy, or the race would have the peer twice (one of them AI-driven).
    if (g_ghost && g_ghostKind != GK_RACER && g_replacedRacer == g_ghost && ListContains(VL_AIRACERS, g_ghost)) {
        g_ghostKind = GK_RACER; g_ghostList = VL_AIRACERS; DisableAI(g_ghost);   // same object reused by the race
        LOG("Race: the ghost is now the racer built as the peer's car");
    }
    if (g_ghost && g_ghostKind != GK_RACER && g_replacedRacer && g_replacedRacer != g_ghost &&
        ListContains(VL_AIRACERS, g_replacedRacer) && RigidBodyOf(g_replacedRacer)) {
        LOG("Race: switching the ghost to the racer built as the peer's car");
        Game::ReleaseGhost();
    }

    // The peer switched cars: respawn ours as their new model / tuning.
    const CarInfo* pc = Net::PeerCar();
    {   // their car info never came (seen once over a VPN): show them in a car like ours meanwhile
        static CarInfo fallback{}; static DWORD waitSince = 0; static bool told = false;
        if (pc) { waitSince = 0; told = false; }
        else if (!waitSince) waitSince = GetTickCount();
        else if (GetTickCount() - waitSince > 6000) {
            CarInfo me{};
            if (Game::ReadLocalCarInfo(me)) {
                fallback = me; fallback.hasCustom = 0; memset(fallback.custom, 0, sizeof(fallback.custom)); fallback.customHash = me.carKey;
                pc = &fallback;
                if (!told) { told = true; LOG("Ghost: no car info from the other player after 6 s - showing them in a stock car like ours (key %08X)", me.carKey); }
            }
        }
    }
    if (g_ghost && g_ghostKind == GK_SPAWNED && pc && (pc->carKey != g_spawnedKey || pc->customHash != g_spawnedHash)) {
        LOG("Peer changed car - respawning");
        Game::ReleaseGhost();
    }

    if (!g_ghost) {
        void* iv = nullptr;
        // 1) in a race: the racer the factory hook built as the peer's car, else any racer
        if (g_replacedRacer && ListContains(VL_AIRACERS, g_replacedRacer) && RigidBodyOf(g_replacedRacer)) iv = g_replacedRacer;
        // (no "any AI racer" fallback any more: it would take over the rival in a boss race, or an
        //  opponent of a race the other player isn't even in. Without our racer, their car is spawned.)
        if (iv) {
            g_ghostList = VL_AIRACERS;
            g_ghostKind = GK_RACER;
        } else if (GetTickCount() < g_noSpawnUntil) {
            RET("event starting...");
        } else if (pc && GetTickCount() >= g_nextSpawnTry) {
            // 2) free roam: spawn the peer's own car (their model + tuning) where they are
            float dir[3] = { 1, 0, 0 };
            float vlen = sqrtf(latest.s.linVel[0] * latest.s.linVel[0] + latest.s.linVel[2] * latest.s.linVel[2]);
            if (vlen > 1.f) { dir[0] = latest.s.linVel[0] / vlen; dir[2] = latest.s.linVel[2] / vlen; }
            // spawn protection: never create the car inside the local player (same spawn point / grid slot)
            float spawnPos[3] = { latest.s.pos[0], latest.s.pos[1], latest.s.pos[2] };
            if (void* lrb0 = RigidBodyOf(LocalVehicle())) {
                if (Dist(RB_GetVec(lrb0, RB_GetPosition), spawnPos) < 4.f) {   // on our spot: behind us, a little to the left
                    if (!BehindLeftOf(lrb0, spawnPos)) { spawnPos[0] += -dir[2] * 5.f; spawnPos[2] += dir[0] * 5.f; spawnPos[1] += 0.5f; }
                }
            }
            iv = SpawnVehicle(pc->carKey, pc->hasCustom ? pc->custom : nullptr, spawnPos, dir, DRV_REMOTE);
            if (!iv && pc->hasCustom) {
                // Diagnose: is it the customization record that Construct rejects, or the model itself?
                iv = SpawnVehicle(pc->carKey, nullptr, spawnPos, dir, DRV_REMOTE);
                LOG("Spawn of %08X with customization failed; stock retry %s", pc->carKey, iv ? "SUCCEEDED (custom record rejected)" : "failed too");
            }
            if (!iv) {
                // Usually: that car model isn't loaded in this world. Retry later; borrow traffic meanwhile.
                ++g_spawnFails;
                g_ghostStarved = true;
                g_nextSpawnTry = GetTickCount() + 1500;
                LOG("Spawning peer car %08X failed (attempt %d) - freeing room, retry in 1.5 s", pc->carKey, g_spawnFails);
                RET("peer car model not loaded yet - retrying");
            }
            g_ghostList = -1; // any list
            g_ghostKind = GK_SPAWNED;
            g_spawnedKey = pc->carKey;
            g_spawnedHash = pc->customHash;
        } else if (!pc) {
            RET("waiting for peer car info");
        } else {
            // No borrowing of AI traffic cars any more: their AI keeps driving them away.
            RET(g_spawnFails ? "peer car spawn failed - retrying" : "spawning peer car...");
        }
        g_ghost = iv;
        if (g_ghostKind == GK_RACER) DisableAI(iv);
        strncpy_s(g_ghostName, VehName(iv), _TRUNCATE);
        SetCollisions(iv, false);
        g_ghostCollide = false;
        g_separated = false;
        g_protectHoldReset = true;
        LOG("Ghost acquired: %s %p (%s)", g_ghostKind == GK_RACER ? (iv == g_replacedRacer ? "racer built as peer car" : "AI racer")
                                          : g_ghostKind == GK_SPAWNED ? "spawned peer car" : "traffic car", iv, g_ghostName);
    }
    PeerRacerInfoTick();

    void* rb = RigidBodyOf(g_ghost);
    if (!rb) {
        // A car we just spawned gets its physics body a moment later: wait instead of giving up.
        if (g_ghostKind == GK_SPAWNED) {
            static void* loggedFor = nullptr;
            if (loggedFor != g_ghost) {
                loggedFor = g_ghost;
                void* r = Find(g_ghost, Addr::H_IRigidBody);
                LOG("Spawned car %p: IRigidBody %p vtable %08X (accepted: %08X / %08X)", g_ghost, r,
                    r ? (unsigned)VTable(r) : 0u, (unsigned)Addr::VT_RBVehicle_IRigid, (unsigned)Addr::VT_Simple_IRigid);
            }
            RET("peer car loading...");
        }
        Game::ReleaseGhost(); RET("ghost car invalid");
    }

    SetStreamPeer(rb);   // keep the world (and full physics) loaded around the other player too
    if (NisActive()) { g_protectHoldReset = true; ClearCarControls(g_ghost); sprintf_s(out, outLen, "%s  (cutscene)", g_ghostName); return; }
    TakePeerSlotTick(latest, rb);

    // --- target state: snapshot interpolation ~InterpDelayMs in the past (smooth, no guessing) ---
    CarState s = latest.s;
    double extrapMs = 0;
    if (!Net::SampleRemote((double)g_cfg.interpDelayMs, s, &extrapMs)) s = latest.s;

    const float* cur = RB_GetVec(rb, RB_GetPosition);
    float err = Dist(cur, s.pos);

    // Spawn protection: when both games put their players on the same spot (shared safe-house spawn,
    // shared grid slot at an event start), moving the remote car onto it overlaps the two cars and the
    // physics flings them. Until the players have been SafeDistance apart once, keep the remote car
    // where it is (nudged 5 m aside if it is inside the local car) instead of syncing it onto them.
    void* lrbP = RigidBodyOf(LocalVehicle());
    // Re-arm the protection whenever the peer teleports (event start, reset, respawn): its position
    // jumping > 30 m between frames means it may land right on top of us.
    // Either car may teleport first (the two games reach the grid ~0.4 s apart), so watch both and
    // hold the protection for at least 3 s after any teleport before distance may switch it off.
    {
        static float lastTarget[3], lastLocal[3]; static bool haveLast = false;
        static DWORD holdUntil = 0;
        const float* lpos = lrbP ? RB_GetVec(lrbP, RB_GetPosition) : nullptr;
        bool peerJump  = haveLast && Dist(lastTarget, s.pos) > 30.f;
        bool localJump = haveLast && lpos && Dist(lastLocal, lpos) > 30.f;
        // a long jump of the other player (teleported to an event, a milestone pursuit, the safe house):
        // build their car again at the new place once the world there has streamed in, instead of moving
        // the old one there (it came out without a model)
        if (haveLast && Dist(lastTarget, s.pos) > 300.f && g_ghostKind == GK_SPAWNED) {
            LOG("Ghost: the other player jumped %.0f m - building their car again there", Dist(lastTarget, s.pos));
            memcpy(lastTarget, s.pos, 12);
            Game::ReleaseGhost();
            g_nextSpawnTry = GetTickCount() + 1500;
            return;
        }
        if (peerJump || localJump || g_protectHoldReset) {
            holdUntil = GetTickCount() + 3000;
            g_protectHoldReset = false;
            if (g_separated) LOG("Spawn protection re-armed: %s jumped", peerJump ? "peer" : localJump ? "local car" : "new ghost");
            g_separated = false;
        }
        memcpy(lastTarget, s.pos, 12);
        if (lpos) memcpy(lastLocal, lpos, 12);
        haveLast = true;
        g_protectHolding = GetTickCount() < holdUntil;
    }
    if (lrbP && !g_separated) {
        const float* lp = RB_GetVec(lrbP, RB_GetPosition);
        if (!g_protectHolding && Dist(lp, s.pos) > (float)g_cfg.safeDistance) {
            g_separated = true;
            LOG("Spawn protection off: players are %.0f m apart", Dist(lp, s.pos));
        } else if ((g_ghostKind != GK_RACER && !Game::CurrentEventHash()) || Dist(lp, s.pos) < 4.f) {
            // (in any event - also a rival race, where their car isn't a racer - it drives from the first
            // frame and is only held while it would stand inside ours)
            // In a race their car drives from the first frame (collisions stay off until the two are
            // apart): it is only held while it would stand inside ours.
            float zero[3] = { 0, 0, 0 };
            // Blink + ghost only the OTHER player's car (the game's post-reset state), like after a reset:
            // each player sees the other blink and pass through, never themselves. Skipped during
            // cutscenes, because then the local car isn't human-driven and lrbP is null.
            // (no blinking: the user doesn't want it; collisions are off for the ghost during protection)
            if (Dist(lp, cur) < 4.f) {   // standing in our car: put it behind us, a little to the left, facing our way
                float aside[3] = { lp[0] + 5.f, lp[1] + 0.5f, lp[2] };
                BehindLeftOf(lrbP, aside);
                RB_SetVec(rb, RB_SetPosition, aside);
                const float* q = RB_GetVec(lrbP, RB_GetOrientation);
                float qq[4] = { q[0], q[1], q[2], q[3] };
                VCall<void(__thiscall*)(void*, const float*)>(rb, RB_SetOrientationQuat)(rb, qq);
            }
            RB_SetVec(rb, RB_SetLinearVelocity, zero);
            RB_SetVec(rb, RB_SetAngularVelocity, zero);
            ClearCarControls(g_ghost);
            sprintf_s(out, outLen, "%s  spawn protection (until %d m apart)", g_ghostName, g_cfg.safeDistance);
            return;
        }
    }

    // Hard-set the exact interpolated transform + velocities every frame. Because consecutive targets
    // come from one smooth curve, the car moves smoothly; setting velocities makes the physics step
    // between our frames carry it along the same path instead of fighting us.
    RB_SetVec(rb, RB_SetOrientationQuat, s.rot);
    RB_SetVec(rb, RB_SetPosition, s.pos);
    RB_SetVec(rb, RB_SetLinearVelocity, s.linVel);
    RB_SetVec(rb, RB_SetAngularVelocity, s.angVel);
    float* newPos = s.pos;
    if (g_cfg.syncControls) ApplyCarControls(g_ghost, s);
    ApplyCarDamage(g_ghost, s);

    // jitter meter: how far each frame's position is from a straight-line prediction made from the
    // previous two frames, scaled by their frame times (so uneven frame rates don't count). RMS, mm.
    {
        static float p1[3], p2[3]; static double t1 = 0, t2 = 0; static int have = 0;
        static double acc = 0; static int n = 0; static float rms = 0;
        double t0 = Net::LocalClockMs();               // simulation time of this frame
        if (have >= 2 && t1 > t2 && t0 > t1) {
            double r = (t0 - t1) / (t1 - t2);
            float d2 = 0;
            for (int i = 0; i < 3; ++i) { float pred = p1[i] + (float)((p1[i] - p2[i]) * r); float e = newPos[i] - pred; d2 += e * e; }
            if (d2 < 1.0f) { acc += d2; ++n; }   // ignore teleports
        }
        memcpy(p2, p1, 12); memcpy(p1, newPos, 12); t2 = t1; t1 = t0; if (have < 2) ++have;
        if (n >= 60) { rms = (float)sqrt(acc / n) * 1000.f; acc = 0; n = 0; }
        g_jitterMm = rms;
        g_extrapMs = (float)extrapMs;
    }

    // --- collisions: off until both cars have been apart once (prevents spawn-overlap soft locks) ---
    float sep = -1.f;
    if (void* lrb = RigidBodyOf(LocalVehicle())) {
        sep = Dist(RB_GetVec(lrb, RB_GetPosition), newPos);
        if (sep > (float)g_cfg.safeDistance && !g_protectHolding) g_separated = true;
        // Momentum for the car we hit: their car is moved by the network here, so our impact only
        // stops US. When our velocity changes abruptly right next to their car (far more than braking
        // can do in one frame), send them what we lost; their game adds part of it to their own car.
        if (g_pushFrom && GetTickCount() - g_pushFrom < 400) {        // being pushed: hold at least the fading push speed
            float f = 1.f - (GetTickCount() - g_pushFrom) / 400.f;
            float plen = sqrtf(g_push[0] * g_push[0] + g_push[2] * g_push[2]);
            if (plen > 0.1f) {
                const float* cv = RB_GetVec(lrb, RB_GetLinearVelocity);
                float ux = g_push[0] / plen, uz = g_push[2] / plen;
                float along = cv[0] * ux + cv[2] * uz, want = plen * f;
                if (along < want) { float nv[3] = { cv[0] + ux * (want - along), cv[1], cv[2] + uz * (want - along) }; RB_SetVec(lrb, RB_SetLinearVelocity, nv); }
            }
        } else g_pushFrom = 0;
        static float prevV[3], hitV0[3]; static DWORD prevT = 0;
        DWORD& lastHitSent = g_lastHitSent; DWORD& hitT0 = g_hitT0;
        const float* lv = RB_GetVec(lrb, RB_GetLinearVelocity);
        DWORD tnow = GetTickCount();
        // an impact takes several frames: note the velocity when it begins, send the total loss 250 ms later
        if ((int)(g_noHitUntil - tnow) > 0) hitT0 = 0;
        if (!hitT0 && (int)(g_noHitUntil - tnow) <= 0 && !g_pushFrom && g_ghostCollide && prevT && tnow - prevT < 100 && sep < 6.f && tnow - lastHitSent > 600) {
            float d0[3] = { prevV[0] - lv[0], 0, prevV[2] - lv[2] };
            if (sqrtf(d0[0] * d0[0] + d0[2] * d0[2]) > 3.f && HitPointsAt(d0, RB_GetVec(lrb, RB_GetPosition), newPos)) { hitT0 = tnow; memcpy(hitV0, prevV, 12); }
        }
        if (hitT0 && tnow - hitT0 >= 250) {
            float dv[3] = { hitV0[0] - lv[0], hitV0[1] - lv[1], hitV0[2] - lv[2] };
            float mag = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
            hitT0 = 0; lastHitSent = tnow;
            if (mag > 4.f) { Net::SendHit(dv); LOG("Collision: hit the other player, lost %.0f km/h - sent to them", mag * 3.6f); }
        }
        memcpy(prevV, lv, 12); prevT = tnow;
    }
    bool wantCollide = g_cfg.ghostCollisions && g_separated;
    if (wantCollide != g_ghostCollide) {
        SetCollisions(g_ghost, wantCollide);
        g_ghostCollide = wantCollide;
        LOG("Ghost collisions %s", wantCollide ? "ON" : "OFF");
    }

    sprintf_s(out, outLen, "%s  jitter %.1fmm  extrap %.0fms  dist %.0fm  err %.1fm  coll %s%s", g_ghostName, g_jitterMm, g_extrapMs, sep, err,
              g_ghostCollide ? "on" : "off",
              (g_cfg.ghostCollisions && !g_separated) ? " (until apart)" : "");
    (void)prev;
#undef RET
}

static bool UpdateGhostSafe(const Net::Snapshot& latest, const Net::Snapshot& prev, bool enabled, char* out, size_t outLen) {
    __try {
        UpdateGhostImpl(latest, prev, enabled, out, outLen);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The other player's car sometimes shows a different 3D model after a while (seen: a traffic station
// wagon) while the game still calls it "cobaltss". Check its render connection once a second:
// FindCarRenderConn(worldId) 0x744D40, worldId = ISimable slot 28 (0x686AF0), conn+0x2C model type,
// conn+0x28 state (2+ = drawn). A changed or missing model -> respawn it. If the model is unchanged, the
// wrong car on screen is another vehicle on the same spot: log what is there.
static int GhostRenderTypeSafe(void* iv) {
    __try {
        void* sim = VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv);
        if (!sim) return -2;
        uint32_t wid = VCall<uint32_t(__thiscall*)(void*)>(sim, 28)(sim);
        char* conn = ((char*(__cdecl*)(uint32_t))0x744D40)(wid);
        if (!conn) return -1;
        if (*(int*)(conn + 0x28) < 2) return -3;                 // still loading
        return *(int*)(conn + 0x2C);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}
static void LogCarsOnGhostSafe() {
    __try {
        void* grb = RigidBodyOf(g_ghost);
        if (!grb) return;
        const float* gp = RB_GetVec(grb, RB_GetPosition);
        void** b = nullptr;
        int n = ListSize(-1, &b);
        for (int i = 0; i < n; ++i) {
            void* v = ListGet(-1, i);
            if (!v || v == g_ghost || v == LocalVehicle()) continue;
            void* rb = RigidBodyOf(v);
            if (rb && Dist(RB_GetVec(rb, RB_GetPosition), gp) < 4.f)
                LOG("Ghost: another car on the other player's spot: %s %p (mirror %d)", VehName(v), v, IsMirrorCar(v) ? 1 : 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void GhostModelCheck() {
    static void* checked = nullptr; static int firstType = -9; static DWORD since = 0, next = 0, nextNear = 0;
    if (!g_ghost || g_ghostKind != GK_SPAWNED) { checked = nullptr; return; }
    DWORD now = GetTickCount();
    if (checked != g_ghost) { checked = g_ghost; firstType = -9; since = now; next = now + 1500; }
    if (now >= nextNear) { nextNear = now + 5000; LogCarsOnGhostSafe(); }
    if (now < next) return;
    next = now + 1000;
    int type = GhostRenderTypeSafe(g_ghost);
    if (type == -3 || type == -2) return;
    if (firstType == -9) { if (type >= 0) { firstType = type; LOG("Ghost: render model type %d", type); } return; }
    if (type != firstType && now - since > 3000) {
        // respawning doesn't help (the new car can get a wrong model at once): only report it, once per change
        LOG("Ghost: render model changed (%d -> %d)", firstType, type);
        firstType = type;
    }
}

std::string Game::UpdateGhost(const Net::Snapshot& latest, const Net::Snapshot& prev, bool enabled) {
    if (!g_ok) return "game not recognised";
    char buf[256] = "";
    if (!UpdateGhostSafe(latest, prev, enabled, buf, sizeof(buf))) {
        LOG("UpdateGhost: exception - releasing ghost");
        g_ghost = nullptr;
        PatchRacerAI(false);
        return "error (see log)";
    }
    GhostModelCheck();
    // Write the ghost line to the log every 2 s so tests don't depend on screenshots.
    static DWORD lastStats = 0;
    if (g_ghost && GetTickCount() - lastStats > 2000) {
        lastStats = GetTickCount();
        LOG("Ghost stats: %s | ping %d ms", buf, Net::Stats().pingMs);
    }
    return buf;
}

static bool DebugSpawnCloneImpl(char* out, size_t n) {
    __try {
        void* me = LocalVehicle();
        void* rb = RigidBodyOf(me);
        if (!rb) { strcpy_s(out, n, "no local car"); return false; }
        const float* p = RB_GetVec(rb, RB_GetPosition);
        const float* q = RB_GetVec(rb, RB_GetOrientation);
        // forward = rotate (0,0,1)? MW world uses +X forward for car bodies in rigid body space;
        // use linear velocity direction if moving, else a fixed offset along X.
        float fwd[3] = { 1, 0, 0 };
        (void)q;
        float pos[3] = { p[0] + fwd[0] * 8.f, p[1] + fwd[1] * 8.f, p[2] + 1.0f };
        // Prefer the peer's (remembered) car: usually a model that isn't loaded here, which exercises
        // the vehicle-cache / budget path. Fall back to a clone of my own car.
        uint32_t key = VehicleKey(me);
        const void* cust = Customizations(me);
        if (const CarInfo* pc = Net::PeerCar()) { key = pc->carKey; cust = pc->hasCustom ? pc->custom : nullptr; }
        void* iv = SpawnVehicle(key, cust, pos, fwd, DRV_REMOTE);
        if (!iv) { sprintf_s(out, n, "spawn failed (key %08X)", key); return false; }
        g_clone = iv;
        sprintf_s(out, n, "spawned %s (key %08X, custom %s)", VehName(iv), key, cust ? "yes" : "no");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(out, n, "exception while spawning");
        return false;
    }
}

std::string Game::DebugSpawnClone() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    char buf[160];
    DebugSpawnCloneImpl(buf, sizeof(buf));
    LOG("DebugSpawnClone: %s", buf);
    return buf;
}

static void SafeKill(void* iv) {
    __try { KillVehicle(iv); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("KillVehicle exception"); }
}

std::string Game::DebugKillClone() {
    if (!g_clone) return "no clone";
    SafeKill(g_clone);
    g_clone = nullptr;
    return "clone removed";
}

std::string Game::DebugInfo() {
    if (!g_ok || GameFlowState() != 6) return "";
    void** b;
    char buf[160];
    sprintf_s(buf, "Cars: players %d  racers %d  cops %d  traffic %d\n",
              ListSize(VL_PLAYERS, &b), ListSize(VL_AIRACERS, &b), ListSize(VL_AICOPS, &b), ListSize(VL_AITRAFFIC, &b));
    return buf;
}
