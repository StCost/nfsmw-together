// Included from game.cpp (after traffic.inl).
// ---------------------------------------------------------------------------
// Pursuits
//  Every player has their own heat and their own pursuit (the game runs it locally); the other
//  player's pursuit cops are shown as mirrors (traffic.inl sends AICOPS both ways).
//  When both players are in a pursuit and the other player's car is close, one cop of OUR pursuit
//  switches its own target to the other player's car (AITarget::Aquire), so cops can go after
//  either player. Heat, busting and evading still track the local player only.
//
//  IVehicle vt+0xAC GetAIVehiclePtr -> IVehicleAI;  IVehicleAI vt+0xA0 GetPursuit -> IPursuit (or null)
//  IVehicleAI vt+0x64 GetTarget -> AITarget (+0x1C ISimable*, +0x2C valid)
//  IPursuit vt+0x08 GetTarget -> AITarget;  cops: IVehicle** at +0x18, count at +0x20
//  AITarget::Aquire(ISimable*) 0x423860 (thiscall). IPerpetrator handle 0x4037E0, vt+4 GetHeat
//
//  [Coop] SharedPursuits (free roam only, see the second half of this file): shared heat, busted
//  together, cooldown only while neither player is seen, the other player's cops on the minimap and
//  the radar detector, ramming one of their cops starts a pursuit here.
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t AquireFn = 0x423860, IPerpetratorHandle = 0x4037E0;
    constexpr float RETARGET_RANGE = 60.f, RELEASE_RANGE = 90.f;

    void* AIOf(void* iv) { return IsVehicle(iv) ? VCall<void* (__thiscall*)(void*)>(iv, 0xAC / 4)(iv) : nullptr; }
    void* PursuitOf(void* iv) {
        void* ai = AIOf(iv);
        return ai ? VCall<void* (__thiscall*)(void*)>(ai, 0xA0 / 4)(ai) : nullptr;
    }
    void* CopTarget(void* cop) {
        void* ai = AIOf(cop);
        return ai ? VCall<void* (__thiscall*)(void*)>(ai, 0x64 / 4)(ai) : nullptr;
    }
    void* SimOf(void* iv) { return iv ? VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv) : nullptr; }
    void Aquire(void* target, void* sim) { ((void(__thiscall*)(void*, void*))AquireFn)(target, sim); }

    void*  g_retargetCop = nullptr;    // the cop of our pursuit currently chasing the other player
    double g_nextPursuitCheck = 0;

    void ReleaseRetarget(void* pursuit) {
        if (!g_retargetCop) return;
        if (ListContains(-1, g_retargetCop) && pursuit) {
            void* pt = VCall<void* (__thiscall*)(void*)>(pursuit, 2)(pursuit);
            void* sim = nullptr;
            if (pt && Mem::Read((uintptr_t)pt + 0x1C, sim) && sim)
                if (void* t = CopTarget(g_retargetCop)) Aquire(t, sim);
        }
        LOG("Pursuit: cop %p back on the local player", g_retargetCop);
        g_retargetCop = nullptr;
    }
}

// Heat x10 of the local player's pursuit, 0 = not in a pursuit. Sent to the peer.
int16_t Game::LocalPursuitHeat() {
    if (!g_ok || Game::GameFlowState() != 6) return 0;
    __try {
        void* me = LocalVehicle();
        if (!PursuitOf(me)) return 0;
        float heat = 1.f;
        void* perp = Find(me, IPerpetratorHandle);
        if (perp) heat = VCall<float(__thiscall*)(void*)>(perp, 1)(perp);
        int h = (int)(heat * 10.f + 0.5f);
        return (int16_t)(h < 1 ? 1 : h);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void PursuitTickImpl(bool connected, const Net::Snapshot& peer) {
    double now = Net::LocalClockMs();
    if (g_nextPursuitCheck - now > 1000) g_nextPursuitCheck = now;   // sim clock restarted
    if (now < g_nextPursuitCheck) return;
    g_nextPursuitCheck = now + 400;
    void* me = LocalVehicle();
    void* pursuit = (connected && Game::GameFlowState() == 6) ? PursuitOf(me) : nullptr;
    void* other = g_ghost;
    {   // one status line every 5 s while either of us is being chased
        static DWORD nextLog = 0;
        int n0 = 0; if (pursuit) Mem::Read((uintptr_t)pursuit + 0x20, n0);
        if ((pursuit || (peer.valid && peer.s.pursuitHeat > 0)) && (int)(GetTickCount() - nextLog) > 0) {
            nextLog = GetTickCount() + 5000;
            int mirrors = 0; for (auto& m : g_mirrors) if (m.used && m.iv && m.n && (m.s[m.n - 1].e.flags & 1)) ++mirrors;
            LOG("Pursuit: mine %s (%d cops, heat %.1f) | theirs heat %.1f, %d of their cops shown here",
                pursuit ? "yes" : "no", n0, Game::LocalPursuitHeat() / 10.f, peer.valid ? peer.s.pursuitHeat / 10.f : 0.f, mirrors);
        }
    }
    bool peerInPursuit = peer.valid && peer.s.hasCar && peer.s.pursuitHeat > 0;
    void* otherRb = RigidBodyOf(other);
    if (!pursuit || !peerInPursuit || !otherRb) { ReleaseRetarget(pursuit); return; }

    const float* op = RB_GetVec(otherRb, RB_GetPosition);
    void** cops = nullptr; int n = 0;
    Mem::Read((uintptr_t)pursuit + 0x18, cops);
    Mem::Read((uintptr_t)pursuit + 0x20, n);
    if (!cops || n <= 1) { ReleaseRetarget(pursuit); return; }    // keep a lone cop on us

    // keep the current one while it is in our pursuit and close enough
    if (g_retargetCop) {
        bool still = false;
        for (int i = 0; i < n; ++i) if (cops[i] == g_retargetCop) still = true;
        void* rb = still ? RigidBodyOf(g_retargetCop) : nullptr;
        if (!rb || Dist(RB_GetVec(rb, RB_GetPosition), op) > RELEASE_RANGE) ReleaseRetarget(pursuit);
    }
    if (!g_retargetCop) {
        float best = RETARGET_RANGE; void* pick = nullptr;
        for (int i = 0; i < n; ++i) {
            void* rb = RigidBodyOf(cops[i]);
            if (!rb) continue;
            float d = Dist(RB_GetVec(rb, RB_GetPosition), op);
            if (d < best) { best = d; pick = cops[i]; }
        }
        if (!pick) return;
        g_retargetCop = pick;
        LOG("Pursuit: cop %p (%.0f m away) goes after the other player", pick, best);
    }
    // re-apply every check: the pursuit logic may put the cop back on us
    void* t = CopTarget(g_retargetCop);
    void* sim = SimOf(other);
    void* cur = nullptr;
    if (t && sim && Mem::Read((uintptr_t)t + 0x1C, cur) && cur != sim) Aquire(t, sim);
}

static void SharedPursuitTickImpl(bool connected, const Net::Snapshot& peer);

void Game::PursuitTick(bool connected, const Net::Snapshot& peer) {
    if (!g_ok) return;
    __try { SharedPursuitTickImpl(connected, peer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Pursuit: exception in the shared pursuit"); }
    __try { PursuitTickImpl(connected, peer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_retargetCop = nullptr; LOG("Pursuit: exception"); }
}

// Test command 'uncop': remove every police car, so a pursuit can end in an escape (test of
// [UI] SkipPursuitResults on one machine).
static int DebugKillCopsImpl(bool* inPursuit) {
    int killed = 0;
    __try {
        *inPursuit = PursuitOf(LocalVehicle()) != nullptr;
        void** b = nullptr;
        for (int i = ListSize(VL_AICOPS, &b) - 1; i >= 0; --i) {
            void* c = ListGet(VL_AICOPS, i);
            if (c && IsVehicle(c) && c != g_ghost && !IsMirrorCar(c)) { KillVehicle(c); ++killed; }   // not the other player's cops
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return killed;
}
std::string Game::DebugKillCops() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    bool inPursuit = false;
    int n = DebugKillCopsImpl(&inPursuit);
    char buf[80];
    sprintf_s(buf, "removed %d police car(s), pursuit %s", n, inPursuit ? "active" : "none");
    return buf;
}
// Test command 'pursuit': start a pursuit on the local player the way the game's scripts do
// (ForcePursuitStart, 0x60AAC0 cdecl(int minHeatLevel); needs the cop manager *0x92C67C).
static bool DebugForcePursuitImpl() {
    __try {
        void* mgr = nullptr;
        if (!Mem::Read(0x92C67C, mgr) || !mgr) return false;
        ((void(__cdecl*)(int))0x60AAC0)(1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string Game::DebugForcePursuit() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    return DebugForcePursuitImpl() ? "pursuit start requested" : "no cop manager";
}

// ---------------------------------------------------------------------------
// Shared pursuits ([Coop] SharedPursuits=1, free roam only: neither player in an event or a race)
//
//  AIPursuit (ctor 0x442D00) carries IPursuit at +0x48 (vtable 0x892770). The getters in that vtable
//  give the fields below, relative to the IPursuit* that IVehicleAI::GetPursuit returns:
//    +0xDC  busted timer (slot 24 TimeUntilBusted; slot 79 SetBustedTimerToZero writes 0)
//    +0xE8  perp busted (slot 29 IsPerpBusted)   +0xE9 bailed (slot 68 BailPursuit sets it)
//    +0xFC  evade level 0..1 (slot 7)            +0x100 cooldown time left (slot 8)
//    +0x104 cooldown time needed (slot 9 returns it minus 7)
//    +0x114 perp in sight (slot 10)              +0x118 last known position (slot 14)
//    +0x218 status: 0 chase, 1 backup, 2 cooldown, 3 busted, 4 evaded (slot 69)
//  AIPursuit::OnTask (0x443BA0, this = AIPursuit+8, so its offsets are these + 0x40) each sim tick:
//    busted timer += rate; busted when it is above 5.0 (0x890DA4) and not busted / bailed yet: then
//      +0xE8 = 1, status 3 and the arrest message (0x20D60DBF) - the game's own busted flow.
//    +0x128 "time since seen" = min(+0x128 + dt, every cop's IPursuitAI::GetTimeSinceTargetSeen
//      (IPursuitAI 0x4038E0, slot 29)); seen while it is <= 7.0 (0x890858): then +0x124 (search
//      time) = 0 and the last known position = the player's.
//    progress = +0x124 + +0x128; evade level = progress / needed; not seen for 2.5 s in the current
//      state (+0x130 > 0x890D34) starts the cooldown (+0x12C = 1, status 2); evade level >= 1 is the
//      escape (status 4); seen again during the cooldown ends it (status 0). Left = needed - progress.
//  So: Hold = +0x128 and +0x124 to 0 (the pursuit counts us as seen, the cooldown can't start and a
//  running one ends); Delay = lower the progress so our time left is the other player's.
//  Heat: IPerpetrator (handle 0x4037E0) on the player's car (AIPerpVehicle vtable 0x892250): slot 1
//    GetHeat 0x409400 (fld [ecx+0x1C]), slot 2 SetHeat 0x409060 (raises the heat-level events too;
//    Game_SetWorldHeat 0x612660 calls the same slot).
//  Pursuit start: ForcePursuitStart 0x60AAC0 cdecl(int minHeat) (the `pursuit` test command).
//  Minimap: Minimap::DrawCops 0x579E00 thiscall(IVehicle* player), called at 0x59DCFD. It draws
//    VEHICLE_LIST 4 (AICOPS) into 8 icons at Minimap+0xD0, only while the player is in a pursuit,
//    only cops whose IPursuitAI (slot 35) says engaged; colour flashes by Minimap+0xC0 when the cop's
//    slot 5 GetInPursuit. Our mirror cops are not in list 4 and have no IPursuitAI: we draw them into
//    the icons it left hidden (after it ran).
//  Radar detector: 0x6EDE30 thiscall(a, b) (LocalPlayer, called at 0x6F2170) walks list 4 for the
//    nearest cop within 300 m (skips helis, inactive and destroyed cars) and feeds the HUD radar
//    detector (IRadarDetector 0x565A30, its slot 1 also sets 0x91CEF8, which the sound reads). We put
//    our mirror cops into list 4 for the length of that one call.
// ---------------------------------------------------------------------------
namespace {
    constexpr uint32_t P_BustTimer = 0xDC, P_PerpBusted = 0xE8, P_Bailed = 0xE9, P_CoolLeft = 0x100,
                       P_CoolNeed = 0x104, P_Search = 0x124, P_SinceSeen = 0x128, P_Status = 0x218;
    enum { PS_CHASE = 0, PS_BACKUP = 1, PS_COOLDOWN = 2, PS_BUSTED = 3, PS_EVADED = 4 };
    constexpr uintptr_t DrawCopsCall = 0x59DCFD, DrawCopsFn = 0x579E00;
    constexpr uintptr_t RadarCall = 0x6F2170, RadarFn = 0x6EDE30;

    struct SharedState {
        uint16_t heat100 = 0;        // what we send (refreshed every 50 ms)
        uint8_t  flags = 0;
        uint8_t  bustSeq = 0;
        uint16_t left10 = 0;
        SharedPursuit::HeatTracker heat;
        SharedPursuit::BustTracker bust;
        void*    lastBustedPursuit = nullptr;
        DWORD    bustingForPeerAt = 0;      // we bust ourselves because they were busted: not reported back
        DWORD    pendingBustUntil = 0;
        bool     pendingStarted = false;
        int      lastStatus = -2;
        int      lastCool = -1;
        DWORD    lastCopHit = 0;
        int      radarCops = -1, mapCops = -1;
    } g_sp;

    template <typename T> T PRead(void* p, uint32_t off) { T v{}; Mem::Read((uintptr_t)p + off, v); return v; }
    template <typename T> void PWrite(void* p, uint32_t off, T v) { Mem::Write((uintptr_t)p + off, v); }

    bool FreeRoamHere() {
        if (Game::GameFlowState() != 6 || Game::CurrentEventHash() != 0) return false;
        void* rs = nullptr; int mode = 0;
        return !(Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode == 1);
    }
    bool PeerInFreeRoam(const Net::Snapshot& p) {
        return p.valid && p.s.hasCar && p.s.gameFlowState == 6 && p.s.eventHash == 0 && !p.s.inRace;
    }
    bool VehActive(void* iv) { return VCall<bool(__thiscall*)(void*)>(iv, 34)(iv); }        // 0x688200
    bool VehIsHeli(void* iv) {                                                                // class == *0x92C4F4
        const uint32_t* c = VCall<const uint32_t* (__thiscall*)(void*)>(iv, 17)(iv);         // 0x688080
        uint32_t cls = 0, heli = 0;
        return c && Mem::Read((uintptr_t)c, cls) && Mem::Read(0x92C4F4, heli) && cls == heli;
    }
    // A cop of OUR pursuit saw us in the last 7 s (the game's own rule, without any co-op hold)
    bool SeenByOwnCops(void* pursuit) {
        void** cops = nullptr; int n = 0;
        if (!Mem::Read((uintptr_t)pursuit + 0x18, cops) || !Mem::Read((uintptr_t)pursuit + 0x20, n) || !cops || n <= 0 || n > 64) return false;
        for (int i = 0; i < n; ++i) {
            void* c = nullptr;
            if (!Mem::Read((uintptr_t)&cops[i], c) || !IsVehicle(c) || !VehActive(c)) continue;
            if (VCall<bool(__thiscall*)(void*)>(c, 31)(c)) continue;                         // destroyed
            void* pai = Find(c, 0x4038E0);
            if (pai && VCall<float(__thiscall*)(void*)>(pai, 29)(pai) <= SharedPursuit::SEEN_LIMIT_S) return true;
        }
        return false;
    }
    const char* StatusName(int s) {
        switch (s) { case -1: return "none"; case PS_CHASE: return "chase"; case PS_BACKUP: return "backup";
                     case PS_COOLDOWN: return "cooldown"; case PS_BUSTED: return "busted"; case PS_EVADED: return "escaped"; }
        return "?";
    }
    // a mirror of one of the other player's cops (spawned here, driven by their snapshots)
    bool IsPeerCop(const Mirror& m, bool inPursuitOnly) {
        if (!m.used || !m.iv || !m.n || m.racer || m.bound) return false;
        uint16_t f = m.s[m.n - 1].e.flags;
        return (f & 1) && (!inPursuitOnly || (f & 4)) && IsVehicle(m.iv);
    }
}

static void SharedPursuitTickImpl(bool connected, const Net::Snapshot& peer) {
    using namespace SharedPursuit;
    DWORD now = GetTickCount();
    static DWORD next = 0;
    if ((int)(now - next) < 0) return;
    next = now + 50;
    bool on = g_cfg.sharedPursuits && connected && g_coopLinked;
    bool here = on && FreeRoamHere();
    void* me = here ? LocalVehicle() : nullptr;
    void* pursuit = me ? PursuitOf(me) : nullptr;

    // ---- our own pursuit, as the other game sees it ----
    g_sp.heat100 = 0; g_sp.flags = 0; g_sp.left10 = 0;
    bool busted = false, bailed = false;
    int status = -1;
    float left = 0.f;
    if (pursuit) {
        busted = PRead<uint8_t>(pursuit, P_PerpBusted) != 0;
        bailed = PRead<uint8_t>(pursuit, P_Bailed) != 0;
        status = PRead<int>(pursuit, P_Status);
        left = PRead<float>(pursuit, P_CoolLeft);
        if (!busted && !bailed && status != PS_BUSTED && status != PS_EVADED) {
            g_sp.flags |= PURSUIT_ACTIVE;
            if (SeenByOwnCops(pursuit)) g_sp.flags |= PURSUIT_SEEN;
            if (status == PS_COOLDOWN) g_sp.flags |= PURSUIT_COOLDOWN;
            g_sp.left10 = LeftToWire(left);
        }
        if (busted) g_sp.flags |= PURSUIT_BUSTED;
    }
    if (status != g_sp.lastStatus) {
        if (on && (status >= 0 || g_sp.lastStatus >= 0))
            LOG("Pursuit: ours %s -> %s", StatusName(g_sp.lastStatus < -1 ? -1 : g_sp.lastStatus), StatusName(status));
        g_sp.lastStatus = status;
    }
    // ---- 4. busted: count our own busts; the other game follows when the count changes ----
    if (pursuit && busted && pursuit != g_sp.lastBustedPursuit) {
        g_sp.lastBustedPursuit = pursuit;
        g_sp.pendingBustUntil = 0;
        if (g_sp.bustingForPeerAt && now - g_sp.bustingForPeerAt < 15000)
            LOG("Pursuit: busted here too, as the other player was");
        else if (on) {
            ++g_sp.bustSeq;
            LOG("Pursuit: we were busted - the other player gets busted too (count %u)", (unsigned)g_sp.bustSeq);
        }
        g_sp.bustingForPeerAt = 0;
    }
    if (!on) {
        g_sp.heat = {}; g_sp.bust = {}; g_sp.pendingBustUntil = 0; g_sp.lastCool = -1;
        return;
    }
    bool peerHere = PeerInFreeRoam(peer);

    // ---- 1. heat: the hotter of the two, both ways ----
    void* perp = (me && !NisActive()) ? Find(me, IPerpetratorHandle) : nullptr;
    if (perp) {
        float mine = VCall<float(__thiscall*)(void*)>(perp, 1)(perp);
        g_sp.heat100 = HeatToWire(mine);
        float take = peerHere ? HeatToTake(g_sp.heat, mine, peer.s.heat100) : 0.f;
        if (!peerHere) g_sp.heat = {};
        if (take > 0.f) {
            VCall<void(__thiscall*)(void*, float)>(perp, 2)(perp, take);
            g_sp.heat100 = HeatToWire(take);
            LOG("Pursuit: heat %.2f -> %.2f (the other player's heat)", mine, take);
        }
    } else g_sp.heat = {};

    // ---- 4. the other player was busted: bust us too (the game's own arrest) ----
    if (peer.valid && PeerJustBusted(g_sp.bust, peer.s.bustSeq)) {
        if (!here) LOG("Pursuit: the other player was busted - not in free roam here, nothing happens");
        else if (busted) LOG("Pursuit: the other player was busted - we already are");
        else {
            g_sp.pendingBustUntil = now + 8000; g_sp.pendingStarted = false;
            LOG("Pursuit: the other player was busted - %s", pursuit ? "busting us too" : "no pursuit here, starting one to bust us too");
        }
    }
    if (g_sp.pendingBustUntil) {
        if (!here || (int)(now - g_sp.pendingBustUntil) > 0) {
            LOG("Pursuit: gave up busting us (%s)", here ? "no pursuit came" : "left free roam");
            g_sp.pendingBustUntil = 0;
        } else if (pursuit && !busted && !bailed) {
            PWrite<float>(pursuit, P_BustTimer, 10.f);           // above the 5 s limit: busted on the next sim tick
            g_sp.bustingForPeerAt = now;
        } else if (!pursuit && !g_sp.pendingStarted) {
            g_sp.pendingStarted = true;
            DebugForcePursuitImpl();
        }
    }

    // ---- 5. cooldown: only while neither of you is seen, and you escape together ----
    int coolNow = -1;
    if ((g_sp.flags & PURSUIT_ACTIVE) && peerHere) {
        View mine{ true, (g_sp.flags & PURSUIT_SEEN) != 0, status == PS_COOLDOWN, left };
        View theirs = FromWire(peer.s.pursuitFlags, peer.s.coolLeft10);
        Decision d = DecideCooldown(mine, theirs);
        coolNow = (int)d.what;
        if (d.what == Cool::Hold) {
            PWrite<float>(pursuit, P_SinceSeen, 0.f);
            PWrite<float>(pursuit, P_Search, 0.f);
        } else if (d.what == Cool::Delay) {
            float need = PRead<float>(pursuit, P_CoolNeed), search = PRead<float>(pursuit, P_Search), since = PRead<float>(pursuit, P_SinceSeen);
            DelayProgress(need, d.left, search, since);
            PWrite<float>(pursuit, P_Search, search);
            PWrite<float>(pursuit, P_SinceSeen, since);
        }
        if (coolNow != g_sp.lastCool) {
            if (d.what == Cool::Hold) LOG("Pursuit: the other player is still seen - our cooldown waits (%s here)", mine.seen ? "seen" : "out of sight");
            else if (d.what == Cool::Delay) LOG("Pursuit: both out of sight - ours waits for theirs (%.1f s left there, %.1f s here)", theirs.left, mine.left);
            else if (theirs.inPursuit) LOG("Pursuit: %s - cooldown runs as usual", mine.seen ? "we are seen ourselves" : "both out of sight");
        }
    }
    g_sp.lastCool = coolNow;

    // status line every 5 s while either of you is chased
    static DWORD nextLog = 0;
    if ((pursuit || (peer.valid && (peer.s.pursuitFlags & PURSUIT_ACTIVE))) && (int)(now - nextLog) > 0) {
        nextLog = now + 5000;
        uint8_t pf = peer.valid ? peer.s.pursuitFlags : 0;
        LOG("Pursuit: shared - here %s%s, %.1f s left, heat %.2f | there %s%s, %.1f s left, heat %.2f",
            StatusName(status), (g_sp.flags & PURSUIT_SEEN) ? " seen" : "", left, g_sp.heat100 / 100.f,
            (pf & PURSUIT_ACTIVE) ? ((pf & PURSUIT_COOLDOWN) ? "cooldown" : "chase") : (pf & PURSUIT_BUSTED) ? "busted" : "none",
            (pf & PURSUIT_SEEN) ? " seen" : "", peer.valid ? peer.s.coolLeft10 / 10.f : 0.f, peer.valid ? peer.s.heat100 / 100.f : 0.f);
    }
}

// ---- 3. we rammed one of the other player's cops (traffic.inl MirrorHitTick) ----
static void SharedPursuitCopHit(uint16_t mirrorId) {
    if (!g_cfg.sharedPursuits || !g_coopLinked || !FreeRoamHere()) return;
    DWORD now = GetTickCount();
    if (now - g_sp.lastCopHit < 3000) return;
    g_sp.lastCopHit = now;
    if (PursuitOf(LocalVehicle())) { LOG("Pursuit: hit the other player's cop %u - we are already chased", (unsigned)mirrorId); return; }
    bool ok = DebugForcePursuitImpl();
    LOG("Pursuit: hit the other player's cop %u - %s", (unsigned)mirrorId, ok ? "starting a pursuit on us" : "no cop manager, no pursuit");
}

void Game::FillSharedPursuit(CarState& s) {
    s.heat100 = g_sp.heat100;
    s.pursuitFlags = g_sp.flags;
    s.bustSeq = g_sp.bustSeq;
    s.coolLeft10 = g_sp.left10;
}

// ---- 2. the other player's cops on the minimap and the radar detector ----
static void DrawPeerCopsImpl(void* mm, void* playerIv) {
    // which of the 8 cop icons did the game's DrawCops leave free? (the same walk it does)
    uint8_t showAll = 0; Mem::Read(0x91CF00, showAll);
    void* p = PursuitOf(playerIv);
    bool none = !showAll && (!p || VCall<bool(__thiscall*)(void*)>(p, 11)(p));          // IsPursuitBailed
    int slot = 0;
    if (!none) {
        void** b = nullptr;
        int n = ListSize(VL_AICOPS, &b);
        for (int i = 0; i < n && slot < 8; ++i) {
            void* c = ListGet(VL_AICOPS, i);
            if (!IsVehicle(c) || !VehActive(c) || VehIsHeli(c)) continue;
            ++slot;
        }
    }
    int counter = 0; Mem::Read((uintptr_t)mm + 0xC0, counter);
    int drawn = 0;
    for (auto& m : g_mirrors) {
        if (slot >= 8) break;
        if (!IsPeerCop(m, true) || !VehActive(m.iv) || VehIsHeli(m.iv)) continue;
        void* sim = VCall<void* (__thiscall*)(void*)>(m.iv, IV_GetSimable)(m.iv);
        void* icon = nullptr;
        if (!sim || !Mem::Read((uintptr_t)mm + 0xD0 + slot * 4, icon) || !icon) continue;
        float pos[2], dir[2];
        ((void(__cdecl*)(float*, float*, void*))0x579840)(pos, dir, sim);
        ((void(__thiscall*)(void*, float*, float*, void*, bool))0x5675D0)(mm, pos, dir, icon, false);
        uint32_t col = counter <= 2 ? 0xFF0000FF : (counter >= 4 && counter <= 6) ? 0xFFA00000 : 0xFFCCCCCC;   // as a chasing cop
        ((void(__cdecl*)(void*, uint32_t))0x5157E0)(icon, col);
        ++slot; ++drawn;
    }
    if (drawn != g_sp.mapCops) { g_sp.mapCops = drawn; LOG("Minimap: %d of the other player's cops shown", drawn); }
}
void __fastcall hk_DrawCops(void* mm, void*, void* playerIv) {
    ((void(__thiscall*)(void*, void*))DrawCopsFn)(mm, playerIv);
    if (!g_cfg.sharedPursuits || !g_coopLinked) return;
    __try { DrawPeerCopsImpl(mm, playerIv); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Our mirror cops into VEHICLE_LIST 4 (behind the game's own entries, within its capacity) for one call
static int AddPeerCopsToList(uint32_t* oldSize, void** saved) {
    __try {
        uintptr_t bucket = Addr::VehicleLists + VL_AICOPS * Addr::VehicleBucket;
        void** b = nullptr; uint32_t cap = 0, size = 0;
        if (!Mem::Read(bucket + 4, b) || !Mem::Read(bucket + 8, cap) || !Mem::Read(bucket + 0xC, size) || !b || size > cap || cap > 64) return 0;
        int n = 0;
        for (auto& m : g_mirrors) {
            if (n >= 8 || size + n >= cap) break;
            if (!IsPeerCop(m, false) || ListContainsOne(VL_AICOPS, m.iv)) continue;
            saved[n] = b[size + n];
            b[size + n] = m.iv;
            ++n;
        }
        if (n) { *oldSize = size; uint32_t ns = size + n; Mem::Write(bucket + 0xC, ns); }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void RemovePeerCopsFromList(uint32_t oldSize, int n, void** saved) {
    __try {
        uintptr_t bucket = Addr::VehicleLists + VL_AICOPS * Addr::VehicleBucket;
        void** b = nullptr; uint32_t size = 0;
        if (!Mem::Read(bucket + 4, b) || !Mem::Read(bucket + 0xC, size) || !b) return;
        if (size == oldSize + n) {
            for (int i = 0; i < n; ++i) b[oldSize + i] = saved[i];
            Mem::Write(bucket + 0xC, oldSize);
        } else LOG("Radar: the cop list changed during the radar update (%u -> %u) - left as it is", oldSize + n, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void __fastcall hk_RadarDetector(void* self, void*, uint32_t a, uint32_t b) {
    uint32_t oldSize = 0; void* saved[8] = {};
    int added = (g_cfg.sharedPursuits && g_coopLinked) ? AddPeerCopsToList(&oldSize, saved) : 0;
    ((void(__thiscall*)(void*, uint32_t, uint32_t))RadarFn)(self, a, b);
    if (added) RemovePeerCopsFromList(oldSize, added, saved);
    if (added != g_sp.radarCops && (added || g_sp.radarCops > 0)) {
        g_sp.radarCops = added;
        LOG("Radar: %d of the other player's cops in the radar detector", added);
    }
}

// ---------------------------------------------------------------------------
// [Coop] NoPursuitCams: no pursuit-start camera while co-op is linked (the camera stays on your car).
//  When a cop starts a player pursuit (cop code at 0x423E66: IPursuit slot 35 IsPlayerPursuit) it calls
//  0x471BF0 cdecl(IVehicle* cop) at 0x423E71. That function plays the "look at the cop" highlight
//  camera: it gives up when cutscenes are blocked (*0x8EE348 > 0), when 0x469E30 says no (that reads
//  the profile's gameplay option at UserProfile +0x47 - the Options menu camera toggle, called JumpCam
//  in the SDK; the jump camera 0x479900 and the 0x479F80 camera ask it too) or during a NIS, and
//  otherwise hands the cop to the player's camera director (0x471A70 -> 0x469EC0), which then switches
//  to CDActionTrackCop for a moment. Forcing the option off would also be written into the career
//  save at the next autosave, so the call is skipped instead: nothing in the profile changes and the
//  jump camera keeps following the player's own setting.
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t PursuitCamCall = 0x423E71, PursuitCamFn = 0x471BF0;
    void __cdecl hk_PursuitStartCam(void* cop) {
        if (g_cfg.noPursuitCams && g_coopLinked && Net::Stats().connected) {
            static DWORD lastLog = 0;
            if (GetTickCount() - lastLog > 10000) { lastLog = GetTickCount(); LOG("Pursuit: start camera skipped (co-op, NoPursuitCams=1)"); }
            return;
        }
        ((void(__cdecl*)(void*))PursuitCamFn)(cop);
    }
}

bool Game::InstallPursuitHooks() {
    if (g_cfg.noPursuitCams) {
        bool c = PatchCallChecked(PursuitCamCall, PursuitCamFn, (void*)&hk_PursuitStartCam);
        LOG("Pursuit: start camera in co-op %s", c ? "off (NoPursuitCams=1)" : "NOT hooked (call site mismatch) - it still plays");
    }
    if (!g_cfg.sharedPursuits) { LOG("Pursuit: SharedPursuits=0 - each player has their own heat and pursuit"); return false; }
    bool m = PatchCallChecked(DrawCopsCall, DrawCopsFn, (void*)&hk_DrawCops);
    bool r = PatchCallChecked(RadarCall, RadarFn, (void*)&hk_RadarDetector);
    LOG("Pursuit: shared pursuits on (minimap cops %s, radar detector %s)", m ? "hooked" : "NOT hooked", r ? "hooked" : "NOT hooked");
    return m && r;
}
