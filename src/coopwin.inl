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
//
// Finishing as the winner when the other player won (verified in the exe and in the race script).
//  The race flow is a Lua 5.0 state graph ("basicrace") in GLOBAL\gameplay.bin: each handler is a
//  HUFF-packed chunk (unpacked with the exe's own decoder 0x64DCA0). At the finish line,
//  stategraph_basicrace_racing_handler_triggerenter does, for any racer on its last lap:
//      NumberFinished += 1
//      placing = Game.CalculateRanking(Element, NumberFinished)     -- native 0x6058B0
//      Game.NotifyRaceFinished(Element)                              -- native 0x6119F0 (cdecl ISimable*)
//      Game.NotifyRacePlacement(activity, Element, placing)          -- native 0x60AA00
//  NotifyRacePlacement posts MNotifyRacePlacement (kind 0x604440, ctor 0x6044C0(activity key, ISimable*,
//  placing), 0x1C bytes) on channel 0x20D60DBF (Message::Post 0x654AF0 copies it). Its handlers:
//    racing_handler_notifyraceplacement_3: a human racer, all humans done -> Placing == 1 ?
//        ChangeState("postrace") (EnterPostRaceFlow 0x611F20 -> ShowPostRace(true) / the rival flow)
//      : ChangeState("raceover") (the losing screen, whose Continue the script then waits for)
//    race_bin_rival_unlocked_handler_notifyraceplacement: Placing <= 1 and Activity is one of the
//        rival's BossRaces -> NumRivalRacesWon += 1; all won -> "done" (rival beaten).
//  So the winner's screen, the rival progress and the payout (in NotifyRaceFinished) all follow the
//  placing the script gets. We (1) answer CalculateRanking with 1 for our racer once the other player
//  has won (a racer of theirs that crossed our line first must not make us 2nd), and (2) when their
//  win arrives while we still drive, do the script's two calls ourselves for our racer with placing 1:
//  the state graph moves on exactly as if we had crossed the line first, nothing is left waiting.
//  Activity key of the race: what GRaceStatus::SetRaceActivity (0x6031F0) hands GRaceDatabase 0x600A20,
//  which finds the race parameters by it: parms+4 ? *(parms+4) : *(*(*(parms+8)+4)+0x20). We check that
//  0x600A20 gives our parameters back before posting.
// ---------------------------------------------------------------------------
// The prologue's last race (16.2.1 against Razor) is a scripted loss: no shared win there.
static bool ScriptedLossEvent() {
    const char* id = EventIdOf(CurrentParms());
    return id && strcmp(id, "16.2.1") == 0;
}
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
        if (ScriptedLossEvent()) return false;      // the prologue's Razor race: the story needs the loss
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
    // (never in the prologue's Razor race 16.2.1: the story needs that loss - the car breaks down on both)
    if (ev && connected && sameEvent && peer.s.inRace && peer.s.finished == 1 && g_peerRacedHere && !ScriptedLossEvent()) g_peerWonEvent = ev;   // also without a ghost racer (rival races)
}

// ---- finish our racer as the winner (see the top of this file) ----
namespace {
    bool     g_forcedFinishTried = false;
    uint32_t g_forcedFinishEvent = 0;

    bool PeerWonThisEvent() {
        return g_cfg.sharedWins && g_coopLinked && g_peerWonEvent && g_peerWonEvent == Game::CurrentEventHash();
    }
    void* LocalSimable() {
        void* iv = LocalVehicle();
        return IsVehicle(iv) ? VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv) : nullptr;
    }
    // the key the race script's activity is known by (see the top of this file); 0 if unknown
    uint32_t ActivityKeyOf(void* parms) {
        void* idx = nullptr; void* a = nullptr; void* b = nullptr; uint32_t key = 0;
        if (!parms || !Mem::Read((uintptr_t)parms + 4, idx)) return 0;
        if (idx) return Mem::Read((uintptr_t)idx, key) ? key : 0;
        if (!Mem::Read((uintptr_t)parms + 8, a) || !a || !Mem::Read((uintptr_t)a + 4, b) || !b) return 0;
        return Mem::Read((uintptr_t)b + 0x20, key) ? key : 0;
    }
    // MNotifyRacePlacement(activity, racer, placing) posted like the native NotifyRacePlacement does
    bool PostRacePlacement(void* parms, void* sim, int placing) {
        void* db = nullptr;
        uint32_t key = ActivityKeyOf(parms);
        if (!key || !Mem::Read(0x91E004, db) || !db) return false;
        void* back = ((void*(__thiscall*)(void*, uint32_t))0x600A20)(db, key);
        if (back != parms) { LOG("Race: activity key %08X does not lead back to this race (%p, not %p) - not posting the placement", key, back, parms); return false; }
        // the message carries the racer's handle (*(ISimable+8)), like the native NotifyRacePlacement 0x60AA00
        uint32_t handle = 0;
        if (!Mem::Read((uintptr_t)sim + 8, handle) || !handle) { LOG("Race: our racer has no handle - not posting the placement"); return false; }
        alignas(4) uint8_t msg[0x20] = {};
        ((void*(__thiscall*)(void*, uint32_t, uint32_t, int))0x6044C0)(msg, key, handle, placing);
        ((void(__thiscall*)(void*, uint32_t))0x654AF0)(msg, 0x20D60DBF);
        return true;
    }
    bool IsLocalRacerSim(void* sim) {
        void* rs = nullptr;
        if (!sim || !Mem::Read(0x91E000, rs) || !rs) return false;
        void* ri = ((void*(__thiscall*)(void*, void*))0x5E8B20)(rs, sim);
        return ri && ri == RacerInfoOfLocal();
    }

    // Lua native CalculateRanking (0x6058B0, cdecl (ISimable*, int numberFinished) -> placing). Its first
    // instruction is `call 0x6E8DF0` (5 bytes, replaced by our jmp): the trampoline makes that call and
    // goes on at 0x6058B5 with the caller's stack untouched.
    __declspec(naked) int __cdecl CalcRankingOrig(void*, int) {
        __asm {
            mov eax, 0x6E8DF0
            call eax
            push 0x6058B5
            ret
        }
    }
    int __cdecl hk_CalcRanking(void* element, int numberFinished) {
        int r = CalcRankingOrig(element, numberFinished);
        __try {
            bool local = IsLocalRacerSim(element);
            int placing = CoopRace::PlacingForScript(local, local && PeerWonThisEvent(), r);
            if (placing != r) { LOG("Race: we crossed the line %d. - the other player won, so the race script gets place 1", r); r = placing; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return r;
    }
}

static void CoopFinishTickImpl() {
    uint32_t ev = Game::CurrentEventHash();
    if (ev != g_forcedFinishEvent || !PeerWonThisEvent()) { g_forcedFinishEvent = ev; g_forcedFinishTried = false; }   // a new win (or restart) may finish us again
    void* rs = nullptr; void* parms = nullptr; int mode = 0;
    if (!Mem::Read(0x91E000, rs) || !rs || !Mem::Read((uintptr_t)rs + 0x1960, mode) || !Mem::Read((uintptr_t)rs + 0x1968, parms) || !parms) return;
    void* ri = RacerInfoOfLocal();
    uint8_t fin = 0, timer = 0, restart = 0;
    if (ri) { Mem::Read((uintptr_t)ri + 0x21, fin); Mem::Read((uintptr_t)ri + 0x154, timer); }
    Mem::Read(0x91CF28, restart);
    CoopRace::FinishInputs in{};
    in.sharedWins = g_cfg.sharedWins; in.peerWon = PeerWonThisEvent(); in.racing = mode == 1;
    in.haveLocalRacer = ri != nullptr; in.localFinished = fin != 0; in.timerRunning = timer != 0;
    in.restartPending = restart != 0; in.cutscene = NisActive(); in.alreadyTried = g_forcedFinishTried;
    in.raceType = in.peerWon ? ((int(__thiscall*)(void*))0x5FAA20)(parms) : -1;
    if (!CoopRace::ShouldFinishAsWinner(in)) return;
    {   // not under the pause menu / map (co-op keeps the race running there), and not before we drove off
        int req = 0; Mem::Read(0x91CAE4, req);
        if (req > 0 || FEScreens::IsPushed("Pause_Main.fng") || FEScreens::IsPushed("WorldMapMain.fng")) return;   // next tick
        float pct = 0.f; if (ri) Mem::Read((uintptr_t)ri + 0x18, pct);
        if (!(pct > 1.0f)) return;
    }
    g_forcedFinishTried = true;
    void* sim = LocalSimable();
    if (!sim) return;
    LOG("Race: the other player won while we still drive - finishing our racer as the winner (race type %d)", in.raceType);
    ((void(__cdecl*)(void*))0x6119F0)(sim);                      // NotifyRaceFinished: finished, paid, car to the AI
    int rank = 0; fin = 0;
    Mem::Read((uintptr_t)ri + 0x21, fin); Mem::Read((uintptr_t)ri + 0x10, rank);
    bool posted = PostRacePlacement(parms, sim, 1);              // the script's next step: place 1 -> winning flow
    LOG("Race: our racer finished %d (rank %d here), placement 1 %s", (int)fin, rank,
        posted ? "posted - the race script goes on to the winner's screen" : "NOT posted - the race ends when the game's AI drives our car over the line");
    if (posted) g_coopWinNotice = true;
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
    // before their racer is marked finished here: our racer then sorts first (rank 1) as well
    __try { if (connected) CoopFinishTickImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Race: exception finishing our racer"); }
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
    // Instead our racer gets place 1 from the race script's own CalculateRanking once the other player
    // won, and is finished as the winner when their win arrives while we drive (CoopFinishTickImpl).
    const uint8_t calc[5] = { 0xE8, 0x3B, 0x35, 0x0E, 0x00 };   // call 0x6E8DF0
    bool d = PatchJmp(0x6058B0, calc, 5, (void*)&hk_CalcRanking);
    LOG("Career: shared wins %s/%s/%s, winner's place for the race script %s", a ? "ok" : "FAIL", b ? "ok" : "FAIL", c ? "ok" : "FAIL", d ? "ok" : "FAIL (code differs)");
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
            // A car put on ground the game hasn't loaded is moved back to its old road a moment later
            // (seen in two-PC tests): load the world there first (as `tp`), park on the next `ram`.
            static float loadAt[3] = { 1e9f, 1e9f, 1e9f }; static DWORD loadedAt = 0;
            bool loaded = Dist(loadAt, park) < 50.f && GetTickCount() - loadedAt >= 3000 && GetTickCount() - loadedAt < 60000;
            StreamHoldAt(park, 30000);
            if (!loaded) {
                memcpy(loadAt, park, 12); loadedAt = GetTickCount();
                sprintf_s(out, n, "loading the world next to %s - send ram%s again in 5 s", cop ? "their cop" : "the other player", cop ? " cop" : "");
                return;
            }
            loadedAt = 0;
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
    // Verified in the exe: 0x4255A0 has this one caller, and every racer AI - the Blacklist rival too -
    // gets its catch-up only through 0x4293D0 (vtable 0x8923C0; called from 0x42AAD4 and 0x42F1D5): the
    // gap to the players feeds a PID (0x653E20 on AI+0x7B0), whose output GRaceStatus 0x5FF990 turns into
    // the AI's skill (AI+0x7B8 / +0x7AC). The rival-only branches there (race flags 0x5FC800 / 0x5FBD90:
    // the race's own catch-up tables, a higher floor for a boss) only reshape that output; they measure
    // nothing themselves. The other players-list users in the AI are not about progress (0x428450:
    // career performance matching to the player's car; 0x42C390 / 0x42F570: cops).
    // Rival races: the partner's car isn't a racer here, so their own race % (sent with their state,
    // moved on to now like the standings do) is the measure. A partner whose state drops out for a
    // moment (a lost packet burst, a menu flicker) keeps counting for 3 s, so the AI never falls back to
    // waiting for the player behind in between.
    bool LeaderPct(float& out) {
        __try {
            void* mine = RacerInfoOfLocal();
            float a = 0, b = 0;
            if (!mine || !Mem::Read((uintptr_t)mine + 0x18, a)) return false;
            const Net::Snapshot& p = Net::Latest();
            void* gri = GhostRacerInfo();
            uint32_t ev = Game::CurrentEventHash();
            // a partner who already finished is out of the race: the AI goes back to racing just us
            bool linked = Net::Stats().connected && g_coopLinked;
            bool peerRacing = linked && p.valid && p.s.inRace && !p.s.finished && p.s.eventHash && p.s.eventHash == ev;
            static DWORD lastOk = 0; static uint32_t lastOkEvent = 0; static float lastPeer = 0;
            DWORD now = GetTickCount();
            bool peerOk = false;
            const char* how = "";
            uint8_t restart = 0; Mem::Read(0x91CF28, restart);   // a restart is pending: no old values
            if (peerRacing && gri && Mem::Read((uintptr_t)gri + 0x18, b)) { peerOk = true; }   // their racer slot (WritePeerProgress keeps it current)
            else if (peerRacing && !gri) {                                                                 // rival races: their car isn't a racer here
                b = p.s.racePct;
                int ping = Net::Stats().pingMs;
                Net::RemoteRacePctNow(ping > 0 && ping < 1000 ? ping * 0.5 : 0.0, b);
                peerOk = true; how = ", rival race";
            } else if (linked && lastOk && ev && ev == lastOkEvent && now - lastOk < 3000 && !(p.valid && p.s.finished) && !restart) {
                b = lastPeer; peerOk = true; how = ", partner's last value";                               // a short gap in their state
            }
            if (peerOk && peerRacing) { lastOk = now; lastOkEvent = ev; lastPeer = b; }
            if (!peerOk) {
                if (linked && ev) {                                                                        // why the partner doesn't count (diagnostics)
                    static DWORD lastSoloLog = 0;
                    if (now - lastSoloLog > 15000) {
                        lastSoloLog = now;
                        LOG("Race: catch-up measured against us only (ours %.1f%%; partner state %d, in race %d, finished %d, event %08X, ours %08X)",
                            a, (int)p.valid, (int)p.s.inRace, (int)p.s.finished, p.s.eventHash, ev);
                    }
                }
                return false;                                                                      // solo: leave the game alone
            }
            NetUtil::CatchUpLeaderPct(a, true, b, out);
            static DWORD lastLog = 0;
            if (now - lastLog > 15000) { lastLog = now; LOG("Race: catch-up measured against %.1f%% (ours %.1f%%, partner %.1f%%%s)", out, a, b, how); }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    PctAvg* __cdecl hk_PlayersPct(PctAvg* out, int list, float s0, float c0) {
        float pct = 0;
        if (g_cfg.catchUpLeader && list == 1 && LeaderPct(pct)) { out->sum = pct; out->cnt = 1.0f; return out; }
        return ((AvgFn)0x4255A0)(out, list, s0, c0);
    }
}

// ---------------------------------------------------------------------------
// Standings: the other player's place.
//  GRaceStatus update 0x602C80 (racing only), once per sim step: for each racer slot (array at
//  GRaceStatus+0x18, 0x194 bytes each) it calls GRacerInfo update 0x5E92E0 (call at 0x602D67), which
//  recomputes race % (+0x18) and lap % (+0x24) from laps (+0x28), checkpoints this lap (+0x2C) and the
//  car's remaining distance to the next checkpoint (IVehicleAI 0x403700, vtable +0x60); then it sorts
//  (SortRacers 0x5FDE80; compare 0x5E31D0: finished first, then more laps, then higher race %) and
//  hands out ranks (+0x10). All verified in the exe.
//  The other player's racer has its racing AI switched off (we drive it), so that AI distance is not
//  kept up and their % was mostly worked out from the checkpoints they had passed: they only moved up
//  when they reached the next checkpoint (seen: ~100 m ahead before the order changed). Our own write
//  of their % came after the sort (at Present), so it never decided the order.
//  Now, right after the game updates their slot and before the sort, we put in the % their own game
//  computed (the same formula on the same route, so both PCs use one scale), moved on to this moment
//  by their recent rate (NetUtil::RacePctAt), plus their laps and checkpoints.
//  Why their row takes an AI opponent's slot instead of being one extra row: the race script reads
//  Game.GetNumRacers() once (basicrace initial state, right after InitRacers 0x61DBE0, whose count is
//  1 - humans + GRaceParameters 0x5FACD0 and whose opponents come from 0x5FD2D0(i)) and sizes its
//  per-racer tables (InRace, GoalList, NextGoal, LapsLeft) by it. Its checkpoint / finish handler looks
//  every car that enters a trigger up with GetRacerIndex and indexes those tables: a racer slot added
//  after that has no entries (a Lua error at their first checkpoint), and one more opponent before it
//  needs an opponent record the event doesn't have (0x5FD2D0(N)) plus a grid slot. Not done.
// ---------------------------------------------------------------------------
namespace {
    // the other player's progress into their racer slot; false when it isn't theirs to write
    bool WritePeerProgress(void* ri, float* usedPct = nullptr) {
        const Net::Snapshot& p = Net::Latest();
        if (!ri || !Net::Stats().connected || !g_coopLinked || !p.valid || !p.s.inRace || !p.s.eventHash ||
            p.s.eventHash != Game::CurrentEventHash()) return false;
        uint8_t fin = 0;
        if (!Mem::Read((uintptr_t)ri + 0x21, fin) || fin) return false;   // finished: CoopWinTick owns the slot
        float pct = p.s.racePct;
        int ping = Net::Stats().pingMs;
        Net::RemoteRacePctNow(ping > 0 && ping < 1000 ? ping * 0.5 : 0.0, pct);   // to now: + their one-way delay
        int laps = p.s.laps, cps = p.s.checkpoints;
        float lapPct = p.s.lapPct;
        Mem::Write((uintptr_t)ri + 0x18, pct);
        Mem::Write((uintptr_t)ri + 0x24, lapPct);
        Mem::Write((uintptr_t)ri + 0x28, laps);
        Mem::Write((uintptr_t)ri + 0x2C, cps);
        if (usedPct) *usedPct = pct;
        return true;
    }
    using RacerUpdateFn = void(__thiscall*)(void*, float);
    void __fastcall hk_RacerUpdate(void* ri, void*, float dt) {
        ((RacerUpdateFn)0x5E92E0)(ri, dt);
        __try {
            if (ri && g_ghost && g_ghostKind == GK_RACER && ri == GhostRacerInfo()) WritePeerProgress(ri);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

bool Game::InstallCatchUp() {
    bool st = PatchCallChecked(0x602D67, 0x5E92E0, (void*)&hk_RacerUpdate);
    LOG("Race: standings use the other player's own progress %s", st ? "hooked" : "NOT hooked (call site differs)");
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
