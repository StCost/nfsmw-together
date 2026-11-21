// Included from game.cpp (after coopwin.inl).
// ---------------------------------------------------------------------------
// Shared milestones ([Career] SharedMilestones): when one player completes a milestone (pursuit length,
// bounty, cost to state, infractions, tags, roadblocks, spike strips, speed trap totals...) or one of the
// rival's speed traps, the other player's career gets it too, through the game's own award (bounty and
// all), if their career is at the same Blacklist rival. Decisions: milestoneutil.h (tested).
//
// Game side (speed.exe v1.3, read from the code):
//  GManager *0x91E00C: milestones +0xA4 count / +0xA8 array, speed traps +0xAC count / +0xB0 array,
//    20-byte entries (layout in milestoneutil.h). Bin start 0x62C330 writes the rival number to
//    UserProfile+0xB0 and switches that bin's milestones 1 -> 2 (0x5E46F0) and speed traps |= 3 (0x5E9D60).
//  GMilestone::NotifyProgress 0x5DE4B0 (thiscall, float value): state 2 and the goal met -> "milestone
//    reached" popup (0x641C20), state 3. Called by GManager 0x5EDA20 (milestone type value changed,
//    free roam only).
//  GMilestone award 0x6015B0 (thiscall, bool award): state 3 -> 4 when award (records +0x10 = the type's
//    last value, or the goal when +9 bit 2 is set; MNotifyMilestoneReached; bounty 0x612220 from the
//    milestone's attribute 0x8E1904C7; rival stats 0x6010E0), back to 2 when not (busted). The pursuit
//    end calls it for every open milestone (0x602770).
//  GSpeedTrap complete 0x601730 (thiscall, float speed m/s): unlocked + active -> done, records the speed,
//    rival stats, re-arms the remaining traps. Called from the speed trap trigger 0x60A7B0.
// We find our completions by reading both lists 4x a second (open -> done) and send them; a received one
// is applied with the same calls the game makes: NotifyProgress(goal) + award(true) / complete(speed).
// ---------------------------------------------------------------------------
namespace {
    namespace SM = SharedMilestones;
    constexpr uintptr_t MsNotifyProgress = 0x5DE4B0;
    constexpr uintptr_t MsAward          = 0x6015B0;
    constexpr uintptr_t SpeedTrapComplete = 0x601730;
    constexpr int MS_ENTRY = 20;

    bool g_msReady = false;                 // code checked at start-up
    SM::Tracker g_msTracker;
    SM::Outbox  g_msOut;
    SM::Inbox   g_msIn;
    SM::KeyRing g_msApplied;                // completed here because the other player did: not sent back
    uint32_t    g_msNextId = 0;
    DWORD       g_msNextPoll = 0;
    char        g_msNotice[128] = "";
    SM::Snap    g_msSnaps[2 * SM::MAX_ENTRIES];

    // one list of GManager: kind 0 milestones, 1 speed traps
    bool MsList(int kind, uint8_t*& arr, int& n) {
        uint8_t* gm = nullptr; uint32_t cnt = 0; arr = nullptr; n = 0;
        uintptr_t off = kind == SM::KIND_MILESTONE ? 0xA4 : 0xAC;
        if (!Mem::Read(0x91E00C, gm) || !gm || !Mem::Read((uintptr_t)gm + off, cnt) || !Mem::Read((uintptr_t)gm + off + 4, arr)) return false;
        if (cnt == 0) { arr = nullptr; return true; }
        if (cnt > (uint32_t)SM::MAX_ENTRIES || !arr || !Mem::IsReadable(arr, cnt * MS_ENTRY)) return false;
        n = (int)cnt;
        return true;
    }
    SM::Snap MsSnapOf(int kind, const uint8_t* e) {
        if (kind == SM::KIND_MILESTONE)
            return SM::FromMilestone(*(const uint32_t*)(e + 4), e[8], *(const uint16_t*)(e + 0xA), *(const float*)(e + 0x10));
        return SM::FromSpeedTrap(*(const uint32_t*)(e + 4), *(const uint16_t*)e, *(const uint16_t*)(e + 2), *(const float*)(e + 0x10));
    }
    // both lists into g_msSnaps (milestones first); -1 = not readable now
    int MsReadAllRaw() {
        int total = 0;
        __try {
            for (int kind = 0; kind < SM::KIND_COUNT; ++kind) {
                uint8_t* arr = nullptr; int n = 0;
                if (!MsList(kind, arr, n)) return -1;
                for (int i = 0; i < n; ++i) g_msSnaps[total++] = MsSnapOf(kind, arr + i * MS_ENTRY);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        return total;
    }
    bool MsFreeRoam() {
        void* rs = nullptr; int mode = -1;
        return Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode == 0 && !NisActive();
    }
    uint8_t MsOurBin() {
        uint8_t* fedb = nullptr; uint8_t* up = nullptr; uint8_t bin = 0;
        if (!Mem::Read(0x91CF90, fedb) || !fedb || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up || !Mem::Read((uintptr_t)up + 0xB0, bin)) return 0;
        return bin;
    }

    // Complete one entry here through the game's own calls. *state = the entry's state / flags after.
    SM::Verdict MsApplyRaw(const SM::Msg& m, uint8_t ourBin, int* stateAfter) {
        *stateAfter = -1;
        __try {
            uint8_t* arr = nullptr; int n = 0;
            if (!MsList(m.kind, arr, n)) return SM::V_NOT_FOUND;
            uint8_t* e = nullptr;
            for (int i = 0; i < n && !e; ++i) if (*(const uint32_t*)(arr + i * MS_ENTRY + 4) == m.key) e = arr + i * MS_ENTRY;
            SM::Snap mine = e ? MsSnapOf(m.kind, e) : SM::Snap{};
            SM::Verdict v = SM::Judge(e != nullptr, mine, m.bin, ourBin);
            if (v != SM::V_APPLY) return v;
            if (m.kind == SM::KIND_MILESTONE) {
                float goal = *(const float*)(e + 0xC);
                uint8_t flags9 = e[9];
                e[9] |= 2;                                                     // record the goal, not our own value
                if (e[8] == SM::MS_AVAILABLE) ((void(__thiscall*)(void*, float))MsNotifyProgress)(e, goal);
                if (e[8] == SM::MS_REACHED) ((void(__thiscall*)(void*, bool))MsAward)(e, true);
                e[9] = (uint8_t)((e[9] & ~2) | (flags9 & 2));
                if (e[8] == SM::MS_DONE && m.value != 0.f) *(float*)(e + 0x10) = m.value;   // what the other player achieved
                *stateAfter = e[8];
            } else {
                float speed = m.value > 0.f && m.value < 200.f ? m.value : *(const float*)(e + 0xC);
                ((void(__thiscall*)(void*, float))SpeedTrapComplete)(e, speed);
                *stateAfter = *(const uint16_t*)e;
            }
            return v;
        } __except (EXCEPTION_EXECUTE_HANDLER) { *stateAfter = -2; return SM::V_NOT_FOUND; }
    }
    bool MsAppliedOk(const SM::Msg& m, int stateAfter) {
        return m.kind == SM::KIND_MILESTONE ? stateAfter == SM::MS_DONE : (stateAfter >= 0 && SM::SpeedTrapDone((uint16_t)stateAfter));
    }
    const char* MsKindName(uint8_t kind) { return kind == SM::KIND_MILESTONE ? "milestone" : "speed trap"; }

    void MsPoll(bool linked) {
        int n = MsReadAllRaw();
        if (n < 0) { g_msTracker.Reset(); return; }
        bool had = g_msTracker.have;
        SM::Msg done[8];
        int k = g_msTracker.Update(g_msSnaps, n, done, 8);
        if (k < 0) {
            if (had) LOG("Milestones: the lists changed (career loaded?) - %d entries, nothing sent for this change", n);
            return;
        }
        for (int i = 0; i < k; ++i) {
            const SM::Msg& m = done[i];
            if (g_msApplied.Has(m.kind, m.key)) continue;              // the other player's, applied here
            if (!linked) { LOG("Milestones: we completed %s %08X (rival %u) - not shared (co-op is off)", MsKindName(m.kind), m.key, m.bin); continue; }
            uint32_t id = ++g_msNextId;
            g_msOut.Push(m, id, GetTickCount());
            LOG("Milestones: we completed %s %08X (rival %u, value %.2f) - telling the other player", MsKindName(m.kind), m.key, m.bin, m.value);
        }
    }

    void MsApplyInbox() {
        g_msIn.DropOld(GetTickCount());
        if (g_msIn.items.empty() || !MsFreeRoam()) return;               // wait for free roam (not a race / cutscene)
        uint8_t ourBin = MsOurBin();
        while (!g_msIn.items.empty()) {
            SM::Msg m = g_msIn.items[0].m;
            int after = -1;
            SM::Verdict v = MsApplyRaw(m, ourBin, &after);
            g_msIn.Done(0);
            if (v != SM::V_APPLY) {
                LOG("Milestones: the other player's %s %08X (rival %u) - %s (our rival %u)", MsKindName(m.kind), m.key, m.bin, SM::VerdictText(v), ourBin);
                continue;
            }
            bool ok = MsAppliedOk(m, after);
            if (ok) g_msApplied.Add(m.kind, m.key);
            LOG("Milestones: the other player's %s %08X (rival %u) %s here (state now %d)", MsKindName(m.kind), m.key, m.bin,
                ok ? "completed" : "NOT completed", after);
            if (ok) sprintf_s(g_msNotice, "%s", m.kind == SM::KIND_MILESTONE ? "completed a milestone" : "passed a speed trap");
        }
    }
}

void Game::MilestoneTick(bool linked) {
    if (!g_ok || !g_msReady || !g_cfg.sharedMilestones) return;
    if (GameFlowState() != 6) { g_msTracker.Reset(); return; }
    DWORD now = GetTickCount();
    if (linked) g_msOut.Flush(now, [](const SM::Msg& m, uint32_t id) { Net::SendMilestone(id, m.kind, m.key, m.bin, m.value); });
    else g_msOut.Clear();
    if ((int32_t)(now - g_msNextPoll) < 0) return;
    g_msNextPoll = now + 250;
    MsPoll(linked);
    MsApplyInbox();
}

// "<who> completed a milestone - ..." once after one was applied here ("" = nothing new)
std::string Game::TakeMilestoneNotice() {
    std::string s = g_msNotice;
    g_msNotice[0] = 0;
    return s;
}

bool Game::InstallSharedMilestones() {
    if (!g_cfg.sharedMilestones) { LOG("Milestones: SharedMilestones=0 - each player keeps their own"); return false; }
    const uint8_t notify[13] = { 0x64, 0xA1, 0, 0, 0, 0, 0x6A, 0xFF, 0x68, 0xFF, 0x4E, 0x87, 0x00 };
    const uint8_t award[13]  = { 0x64, 0xA1, 0, 0, 0, 0, 0x6A, 0xFF, 0x68, 0x60, 0x61, 0x87, 0x00 };
    const uint8_t trap[8]    = { 0x8A, 0x01, 0xF6, 0xD0, 0xA8, 0x01, 0x75, 0x48 };
    if (memcmp((void*)MsNotifyProgress, notify, 13) || memcmp((void*)MsAward, award, 13) || memcmp((void*)SpeedTrapComplete, trap, 8)) {
        LOG("Milestones: NOT shared (game code differs)");
        return false;
    }
    g_msNextId = GetTickCount() * 2654435761u;          // differs after a relaunch
    Net::OnMilestone = [](uint32_t, uint8_t kind, uint32_t key, uint16_t bin, float value) {
        if (!g_cfg.sharedMilestones || !g_coopLinked) return;
        SM::Msg m; m.kind = kind; m.key = key; m.bin = bin; m.value = value;
        if (g_msIn.Push(m, GetTickCount()))
            LOG("Milestones: the other player completed %s %08X (rival %u, value %.2f)", MsKindName(kind), key, bin, value);
    };
    g_msReady = true;
    LOG("Milestones: shared with the other player (same rival only)");
    return true;
}

// ---- test commands ----
static void MsListSafe(char* out, size_t n) {
    int total = MsReadAllRaw();
    if (total < 0) { strcpy_s(out, n, "milestone lists not readable"); return; }
    uint8_t bin = MsOurBin();
    int open[2] = {}, done[2] = {}, cnt[2] = {};
    for (int i = 0; i < total; ++i) {
        const SM::Snap& s = g_msSnaps[i];
        ++cnt[s.kind];
        if (s.open) ++open[s.kind];
        if (s.done) ++done[s.kind];
        if (s.bin == bin && (s.open || s.done))
            LOG("Milestones: %s %08X rival %u %s value %.2f", MsKindName(s.kind), s.key, s.bin, s.done ? "done" : "open", s.value);
    }
    sprintf_s(out, n, "rival %u: milestones %d (%d open, %d done), speed traps %d (%d open, %d done)", bin,
              cnt[0], open[0], done[0], cnt[1], open[1], done[1]);
}
// complete the first open milestone / speed trap of our rival here, as if we had earned it
static void MsCompleteFirstSafe(uint8_t kind, char* out, size_t n) {
    int total = MsReadAllRaw();
    if (total < 0) { strcpy_s(out, n, "milestone lists not readable"); return; }
    uint8_t bin = MsOurBin();
    for (int i = 0; i < total; ++i) {
        const SM::Snap& s = g_msSnaps[i];
        if (s.kind != kind || !s.open || (bin && s.bin != bin)) continue;
        SM::Msg m; m.kind = kind; m.key = s.key; m.bin = s.bin; m.value = 0.f;
        int after = -1;
        SM::Verdict v = MsApplyRaw(m, bin, &after);
        sprintf_s(out, n, "%s %08X (rival %u): %s, state now %d", MsKindName(kind), s.key, s.bin, SM::VerdictText(v), after);
        return;
    }
    sprintf_s(out, n, "no open %s for rival %u", MsKindName(kind), bin);
}
std::string Game::DebugMilestones(const char* arg) {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    char b[160];
    if (!strcmp(arg, "milestone")) MsCompleteFirstSafe(SM::KIND_MILESTONE, b, sizeof(b));
    else if (!strcmp(arg, "speedtrap")) MsCompleteFirstSafe(SM::KIND_SPEEDTRAP, b, sizeof(b));
    else MsListSafe(b, sizeof(b));
    return b;
}
