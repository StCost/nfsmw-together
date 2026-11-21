// Included from game.cpp (after save.inl).
// ---------------------------------------------------------------------------
// Shared wins: when either player finishes an event first, both games count it as won
// (cash, unlocks, "event won" flag, winning post-race flow), each in its own career.
//  NotifyRaceFinished 0x6119F0 pays out only if the finishing local player is ranked 1st:
//    0x611B11  39 6B 10             cmp [ebx+0x10], ebp     ; ebx = local GRacerInfo, ebp = 1
//              0F 85 B4 01 00 00    jne 0x611CCE            ; not first: no payout
//    -> jmp to a stub that asks CoopWinCheck(ebx) and continues at 0x611B1A (win) / 0x611CCE.
//  GRaceStatus "who won" 0x5EB4E0 (thiscall, no args; returns the local GRacerInfo that finished
//    first, else NULL; UpdateRaceScore 0x602320 needs non-NULL to mark the event won) -> replaced.
//  ShowLosingPostRaceScreen 0x60E230 (6A 00 E8 E9 C9 FF FF 59 C3 = ShowPostRace(false)) ->
//    ShowPostRace(co-op won).
//  The other player's racer never "finishes" in our game by itself: when they report finishing, we
//  mark it finished (0x5E97B0, thiscall GRacerInfo), set 100 %, and re-sort (SortRacers 0x5FDE80).
//  The sender's CarState::finished carries its own rank at the finish (0 = still racing).
// ---------------------------------------------------------------------------
namespace {
    bool g_coopWinLogged = false;
    uint32_t g_peerWonEvent = 0;    // event the other player has won (kept until our event changes)
    bool g_coopWinNotice = false;   // main.cpp shows a notice once
    bool g_peerRacedHere = false;   // the other player was seen racing (not finished) in this run of the event:
                                    // a "finished" left over from their previous run of the same event is ignored

    void* GhostRacerInfo() { return (g_ghost && g_ghostKind == GK_RACER) ? RacerInfoOf(g_ghost) : nullptr; }

    bool CoopWonImpl(void* localRi) {
        int rank = 0;
        if (localRi && Mem::Read((uintptr_t)localRi + 0x10, rank) && rank == 1) return true;
        if (!g_cfg.sharedWins) return false;
        if (g_peerWonEvent && g_peerWonEvent == Game::CurrentEventHash()) {
            if (!g_coopWinLogged) { g_coopWinLogged = true; g_coopWinNotice = true; LOG("Race: the other player won - counted as our win too"); }
            return true;
        }
        if (!Net::Stats().connected || !g_coopLinked) return false;
        const Net::Snapshot& p = Net::Latest();
        if (!p.valid || !p.s.inRace || !p.s.eventHash || p.s.eventHash != Game::CurrentEventHash()) return false;   // same event only
        void* gri = GhostRacerInfo();
        // (gri is null in rival races: there the other player's car isn't a racer in our game, and
        // their own result - sent with their state - decides)
        uint8_t fin = 0; int grank = 0;
        if (gri) { Mem::Read((uintptr_t)gri + 0x21, fin); Mem::Read((uintptr_t)gri + 0x10, grank); }
        if (!g_peerRacedHere) return false;         // still showing the result of an earlier run
        if (p.s.finished == 1) g_peerWonEvent = Game::CurrentEventHash();   // remembered: they may leave the race before we finish
        (void)fin; (void)grank;
        bool won = p.s.finished == 1;             // their own result decides (our sort of their car may differ)
        if (won && !g_coopWinLogged) { g_coopWinLogged = true; g_coopWinNotice = true; LOG("Race: the other player won - counted as our win too"); }
        return won;
    }
    bool __cdecl CoopWinCheck(void* localRi) {
        __try { return CoopWonImpl(localRi); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(naked) void hk_WinGate() {
        __asm {
            pushad
            push ebx
            call CoopWinCheck
            add esp, 4
            test al, al
            popad                      ; popad keeps the flags
            jz lose
            push 0x611B1A
            ret
        lose:
            push 0x611CCE
            ret
        }
    }

    void* __fastcall hk_WhoWon(void* raceStatus, void*) {
        __try {
            void* ri = RacerInfoOfLocal();
            uint8_t fin = 0;
            if (ri && Mem::Read((uintptr_t)ri + 0x21, fin) && fin && CoopWonImpl(ri)) return ri;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    void __cdecl hk_ShowLosing() {
        bool won = false;
        __try { won = CoopWonImpl(RacerInfoOfLocal()); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        ((void(__cdecl*)(bool))0x60AC20)(won);                   // ShowPostRace
    }

    bool PatchJmp(uintptr_t at, const uint8_t* expect, size_t len, void* to) {
        uint8_t patch[32];
        if (len < 5 || len > sizeof(patch)) return false;
        memset(patch, 0x90, sizeof(patch));
        patch[0] = 0xE9;
        *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)to - (at + 5));
        return Mem::PatchBytes(at, expect, patch, len);
    }
}

// Rank of the local racer once finished, for the state packet (0 = still racing / not in a race).
uint8_t Game::LocalFinishRank(void* localRi) {
    uint8_t fin = 0; int rank = 0;
    if (!localRi || !Mem::Read((uintptr_t)localRi + 0x21, fin) || !fin) return 0;
    Mem::Read((uintptr_t)localRi + 0x10, rank);
    return (uint8_t)(rank < 1 || rank > 255 ? 255 : rank);   // an unreadable rank is never a win
}

// Forget the other player's win when the event changes, the race is restarted (here or there: their
// "finished" drops back to 0) or we leave to the menus. Runs in every flow state.
static void CoopWinLatchTick(bool connected, const Net::Snapshot& peer) {
    static uint32_t lastEvent = 0; static uint8_t lastPeerFin = 0, lastRestart = 0;
    uint32_t ev = Game::GameFlowState() == 6 ? Game::CurrentEventHash() : 0;
    uint8_t restart = 0; Mem::Read(0x91CF28, restart);
    bool sameEvent = peer.valid && peer.s.eventHash && peer.s.eventHash == ev;
    uint8_t pfin = sameEvent ? peer.s.finished : 0;
    if (ev != lastEvent || (restart && !lastRestart) || (lastPeerFin && !pfin && sameEvent && peer.s.inRace)) {
        g_coopWinLogged = false; g_peerWonEvent = 0; g_peerRacedHere = false;
    }
    lastEvent = ev; lastRestart = restart; lastPeerFin = pfin;
    if (ev && connected && sameEvent && peer.s.inRace && !peer.s.finished) g_peerRacedHere = true;
    if (ev && connected && sameEvent && peer.s.inRace && peer.s.finished == 1 && g_peerRacedHere) g_peerWonEvent = ev;   // also without a ghost racer (rival races)
}

static void CoopWinTickImpl(bool connected, const Net::Snapshot& peer) {
    bool sameEvent = peer.valid && peer.s.eventHash && peer.s.eventHash == Game::CurrentEventHash();
    void* gri = (connected && sameEvent && peer.s.inRace) ? GhostRacerInfo() : nullptr;
    if (!gri || !g_peerRacedHere) return;
    uint8_t fin = 0;
    if (!peer.s.finished || !Mem::Read((uintptr_t)gri + 0x21, fin) || fin) return;
    void* rs = nullptr;
    if (!Mem::Read(0x91E000, rs) || !rs) return;
    ((void(__thiscall*)(void*))0x5E97B0)(gri);                   // finished + race timer frozen
    float full = 100.f; Mem::Write((uintptr_t)gri + 0x18, full);
    ((void(__thiscall*)(void*))0x5FDE80)(rs);                    // SortRacers
    int rank = 0; Mem::Read((uintptr_t)gri + 0x10, rank);
    LOG("Race: the other player finished (rank %d there, %d here)", (int)peer.s.finished, rank);
}

void Game::CoopWinTick(bool connected, const Net::Snapshot& peer) {
    if (!g_ok) return;
    __try { CoopWinLatchTick(connected, peer); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (GameFlowState() != 6) return;
    __try { CoopWinTickImpl(connected, peer); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool Game::InstallCoopWin() {
    if (!g_cfg.sharedWins) return false;
    const uint8_t gate[9] = { 0x39, 0x6B, 0x10, 0x0F, 0x85, 0xB4, 0x01, 0x00, 0x00 };
    const uint8_t who[5]  = { 0xA1, 0x54, 0xD8, 0x92, 0x00 };
    const uint8_t lose[9] = { 0x6A, 0x00, 0xE8, 0xE9, 0xC9, 0xFF, 0xFF, 0x59, 0xC3 };
    // all three or none: a half-installed set would pay out without marking the event won
    if (memcmp((void*)0x611B11, gate, 9) || memcmp((void*)0x5EB4E0, who, 5)) {
        LOG("Career: shared wins NOT installed (code differs)");
        return false;
    }
    bool a = PatchJmp(0x611B11, gate, 9, (void*)&hk_WinGate);
    bool b = PatchJmp(0x5EB4E0, who, 5, (void*)&hk_WhoWon);
    // The losing post-race screen is NOT swapped for the winning one: the race script that asked for it
    // then waits for the losing screen's Continue and the player is stuck at the finish (seen on two PCs).
    // The win is still paid and recorded; the standings table shows who finished first.
    bool c = true; (void)&hk_ShowLosing;
    LOG("Career: shared wins %s/%s/%s", a ? "ok" : "FAIL", b ? "ok" : "FAIL", c ? "ok" : "FAIL");
    return a && b && c;
}

// ---- test commands ----
static int StopOpponentsSafe() {
    int n = 0;
    __try {
        void** b = nullptr;
        int cnt = ListSize(VL_AIRACERS, &b);
        for (int i = 0; i < cnt; ++i) {
            void* r = ListGet(VL_AIRACERS, i);
            if (r && r != g_ghost && !IsMirrorCar(r) && DisableAI(r)) ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
std::string Game::DebugStopOpponents() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    char b[64]; sprintf_s(b, "%d opponent(s) stopped driving", StopOpponentsSafe());
    return b;
}
static void CashSafe(char* out, size_t n) {
    __try {
        uint8_t* fedb = nullptr; uint8_t* up = nullptr; int cash = 0;
        if (!Mem::Read(0x91CF90, fedb) || !fedb || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up) { strcpy_s(out, n, "no profile"); return; }
        Mem::Read((uintptr_t)up + 0xB4, cash);
        int won = -1; void* rs = nullptr; void* parms = nullptr; void* db = nullptr;
        if (Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1968, parms) && parms && Mem::Read(0x91E004, db) && db) {
            uint32_t h = ((uint32_t(__thiscall*)(void*))0x5FAB40)(parms);
            won = ((bool(__thiscall*)(void*, uint32_t, int))0x5DCA70)(db, h, 2) ? 1 : 0;
        }
        sprintf_s(out, n, "cash %d, current event won flag %d", cash, won);
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "exception"); }
}
std::string Game::DebugCash() { char b[96]; CashSafe(b, sizeof(b)); return b; }
bool Game::TakeCoopWinNotice() { bool v = g_coopWinNotice; g_coopWinNotice = false; return v; }

// Test: drive our car into the other player's car (placed 12 m away, moving at 90 km/h towards it),
// and print our own position, to check that a hit pushes the car that was hit on its own PC.
// `ram cop`: the same against the nearest of the other player's police cars shown here (a mirror).
static void RamSafe(char* out, size_t n, bool cop) {
    __try {
        void* me = RigidBodyOf(LocalVehicle()); void* him = cop ? nullptr : RigidBodyOf(g_ghost);
        if (cop && me) {
            float best = 1e9f;
            for (auto& m : g_mirrors) {
                void* rb = (m.used && m.iv && m.n && (m.s[m.n - 1].e.flags & 1)) ? RigidBodyOf(m.iv) : nullptr;
                float d = rb ? Dist(RB_GetVec(rb, RB_GetPosition), RB_GetVec(me, RB_GetPosition)) : 1e9f;
                if (d < best) { best = d; him = rb; }
            }
        }
        if (!me || !him) { strcpy_s(out, n, cop ? "none of the other player's cops shown here" : "no car to ram"); return; }
        const float* hp = RB_GetVec(him, RB_GetPosition);
        const float* mp = RB_GetVec(me, RB_GetPosition);
        float dx = hp[0] - mp[0], dz = hp[2] - mp[2], len = sqrtf(dx * dx + dz * dz);
        if (len < 0.5f) { dx = 1; dz = 0; len = 1; }
        dx /= len; dz /= len;
        // A jump of more than 30 m re-arms spawn protection on both PCs (collisions off for 3 s), so from
        // far away first park 25 m from the other car; the second `ram` (5 s later) then does the hit.
        float zero[3] = { 0, 0, 0 };
        if (len > 30.f) {
            float park[3] = { hp[0] - dx * 25.f, hp[1] + 0.3f, hp[2] - dz * 25.f };
            RB_SetVec(me, RB_SetPosition, park); RB_SetVec(me, RB_SetLinearVelocity, zero); RB_SetVec(me, RB_SetAngularVelocity, zero);
            sprintf_s(out, n, cop ? "parked 25 m from their cop - send ram cop again in 5 s" : "parked 25 m from the other player - send ram again in 5 s");
            return;
        }
        float pos[3] = { hp[0] - dx * 12.f, hp[1] + 0.3f, hp[2] - dz * 12.f }, vel[3] = { dx * 25.f, 0, dz * 25.f };
        g_testJumpOkUntil = GetTickCount() + 400;      // this short hop is the test itself, not a teleport to ignore
        RB_SetVec(me, RB_SetPosition, pos); RB_SetVec(me, RB_SetLinearVelocity, vel); RB_SetVec(me, RB_SetAngularVelocity, zero);
        sprintf_s(out, n, cop ? "ramming the other player's cop from 12 m at 90 km/h" : "ramming the other player from 12 m at 90 km/h");
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "exception"); }
}
std::string Game::DebugRam(bool cop) { char b[96] = "not in game"; if (g_ok && GameFlowState() == 6) RamSafe(b, sizeof(b), cop); return b; }
static void PosSafe(char* out, size_t n) {
    __try {
        void* me = RigidBodyOf(LocalVehicle());
        if (!me) { strcpy_s(out, n, "no car"); return; }
        const float* p = RB_GetVec(me, RB_GetPosition); const float* v = RB_GetVec(me, RB_GetLinearVelocity);
        CarState cs{}; ReadCarControls(LocalVehicle(), cs);
        sprintf_s(out, n, "pos %.1f %.1f %.1f speed %.1f m/s damage %08X lights %08X", p[0], p[1], p[2],
                  sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), cs.damageZones, cs.brokenLights);
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "exception"); }
}
std::string Game::DebugPos() { char b[128] = "not in game"; if (g_ok && GameFlowState() == 6) PosSafe(b, sizeof(b)); return b; }
// ---------------------------------------------------------------------------
// Catch-up ("rubber banding") follows the player who is ahead.
//  Once a second each AI racer measures its gap to the humans: 0x4293D0 calls 0x4255A0(&out, 1, 0, 0)
//  at 0x42956F, which sums GRacerInfo race % (+0x18) over the players list and returns {sum, count};
//  the caller turns it into an average. In co-op the other player is a racer slot, not in that list,
//  so the AI only ever measured itself against the local player. We answer that call ourselves with
//  the progress of whichever human is further along.
// ---------------------------------------------------------------------------
namespace {
    struct PctAvg { float sum, cnt; };
    using AvgFn = PctAvg* (__cdecl*)(PctAvg*, int, float, float);
    bool LeaderPct(float& out) {
        __try {
            void* mine = RacerInfoOfLocal();
            float a = 0, b = 0;
            if (!mine || !Mem::Read((uintptr_t)mine + 0x18, a)) return false;
            const Net::Snapshot& p = Net::Latest();
            void* gri = GhostRacerInfo();
            // a partner who already finished is out of the race: the AI goes back to racing just us
            bool peerRacing = Net::Stats().connected && g_coopLinked && p.valid && p.s.inRace && !p.s.finished &&
                              p.s.eventHash && p.s.eventHash == Game::CurrentEventHash();
            if (peerRacing && gri && Mem::Read((uintptr_t)gri + 0x18, b)) {
                out = b > a ? b : a;
                static DWORD lastLog = 0;
                if (GetTickCount() - lastLog > 15000) { lastLog = GetTickCount(); LOG("Race: catch-up measured against %.1f%% (ours %.1f%%, partner %.1f%%)", out, a, b); }
                return true;
            }
            if (peerRacing && !gri) { out = p.s.racePct > a ? p.s.racePct : a; return true; }   // rival races: their car isn't a racer here
            return false;                                                                      // solo: leave the game alone
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    PctAvg* __cdecl hk_PlayersPct(PctAvg* out, int list, float s0, float c0) {
        float pct = 0;
        if (g_cfg.catchUpLeader && list == 1 && LeaderPct(pct)) { out->sum = pct; out->cnt = 1.0f; return out; }
        return ((AvgFn)0x4255A0)(out, list, s0, c0);
    }
}

bool Game::InstallCatchUp() {
    bool ok = PatchCallChecked(0x42956F, 0x4255A0, (void*)&hk_PlayersPct);
    LOG("Race: catch-up follows the leading player %s", ok ? "hooked" : "NOT hooked (call site differs)");
    return ok;
}
static void TeleportSafe(const float* to, char* out, size_t n) {
    __try {
        void* me = RigidBodyOf(LocalVehicle());
        if (!me) { strcpy_s(out, n, "no car"); return; }
        float pos[3] = { to[0], to[1] + 2.f, to[2] }, zero[3] = { 0, 0, 0 };
        RB_SetVec(me, RB_SetPosition, pos); RB_SetVec(me, RB_SetLinearVelocity, zero); RB_SetVec(me, RB_SetAngularVelocity, zero);
        sprintf_s(out, n, "teleported to %.1f %.1f %.1f", to[0], to[1], to[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "exception"); }
}
// The first 'tp' to a place loads the world there (the game puts a car standing on unloaded ground back
// on its old road); the same 'tp' again a few seconds later moves the car.
std::string Game::DebugTeleport(const char* args) {
    float p[3];
    if (sscanf_s(args, "%f %f %f", &p[0], &p[1], &p[2]) != 3) return "usage: tp x y z (as printed by pos)";
    if (!g_ok || GameFlowState() != 6) return "not in game";
    static float last[3] = { 1e9f, 1e9f, 1e9f }; static DWORD loadedAt = 0;
    bool same = fabsf(last[0] - p[0]) < 1.f && fabsf(last[1] - p[1]) < 1.f && fabsf(last[2] - p[2]) < 1.f;
    StreamHoldAt(p, 30000);
    if (!same || GetTickCount() - loadedAt > 60000) {
        memcpy(last, p, 12); loadedAt = GetTickCount();
        return "loading the world there - send the same tp again in about 5 s";
    }
    char b[96] = "";
    TeleportSafe(p, b, sizeof(b));
    return b;
}
