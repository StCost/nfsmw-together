// Included from game.cpp (uses its vehicle helpers).
// ---------------------------------------------------------------------------
// Shared event start
//  When a player engages a career event (race parameters appear while roaming), their game sends the
//  event ID. The other game (if it's in free roam) looks the event up, teleports its car onto the
//  event's start marker and accepts the "engage event" dialog through Macro (input.cpp), so both
//  load the same event through the game's own flow (career bookkeeping stays intact).
// Verified/known addresses (v1.3):
//  GRaceStatus::fObj          *(0x91E000)   +0x1960 mPlayMode (0 roaming, 1 racing)
//                                            +0x1964 mRaceContext (0 quick race, 1 online, 2 career)
//                                            +0x1968 mRaceParms (GRaceParameters*)
//  GRaceDatabase::mObj        *(0x91E004)
//  GRaceDatabase::GetRaceFromHash   0x5FB710  thiscall(db, uint32 hash) -> GRaceParameters*
//  GRaceParameters::GetEventID      0x5FBA70  thiscall(parms) -> const char*
//  GRaceParameters::GetStartPosition 0x5FAD10 thiscall(parms, Vector3* out)
// ---------------------------------------------------------------------------
namespace {
    void*    g_lastParms = nullptr;          // race parameters we last saw on this machine
    uint32_t g_handledHash = 0; DWORD g_handledAt = 0;   // invitation we already acted on (repeats are ignored)
    uint32_t g_curEventHash = 0;             // event we are in (0 = none), sent with our state
    uint32_t g_joiningEvent = 0;             // event hash we're joining because the peer started it
    DWORD    g_joinStartedAt = 0;
    char     g_eventStatus[96] = "";

    void* RaceStatus() { void* p = nullptr; Mem::Read(0x91E000, p); return p; }
    void* CurrentParms(int* playMode = nullptr, int* context = nullptr) {
        void* rs = RaceStatus();
        if (!rs) return nullptr;
        void* parms = nullptr;
        Mem::Read((uintptr_t)rs + 0x1968, parms);
        if (playMode) Mem::Read((uintptr_t)rs + 0x1960, *playMode);
        if (context) Mem::Read((uintptr_t)rs + 0x1964, *context);
        return parms;
    }
    const char* EventIdOf(void* parms) {
        if (!parms) return nullptr;
        const char* id = ((const char*(__thiscall*)(void*))0x5FBA70)(parms);
        return (id && Mem::IsReadable(id, 1)) ? id : nullptr;
    }
    uint32_t HashName(const char* s) { return ((uint32_t(__cdecl*)(const char*))0x4519D0)(s); }

    // Challenge Series events run with race context 0 like quick races, but only they have a preset
    // player car (GRaceParameters::GetPlayerCarType 0x5FC5C0 returns a non-empty name).
    bool IsChallengeEvent(void* parms) {
        if (!parms) return false;
        const char* car = ((const char*(__thiscall*)(void*))0x5FC5C0)(parms);
        return car && Mem::IsReadable(car, 2) && (unsigned char)car[0] >= 0x20 && (unsigned char)car[0] < 0x7F;   // a printable name
    }
    uint32_t EventHashOf(void* parms) {           // GRaceParameters::GetEventHash (0x5FAB40)
        return parms ? ((uint32_t(__thiscall*)(void*))0x5FAB40)(parms) : 0;
    }

    // Start an event the way the game does (found by RE of the Safe House race sheet / Blacklist):
    //  free roam (flow 6, roaming): 0x5FDCB0 thiscall(GManager = *(0x91E00C), eventHash)
    //  front end (flow 3):          db = *(0x91E004); c = db->0x603440(parms); db->0x600AB0(c, 2 /*career*/);
    //                               db->0x5E39E0(c); 0x57ECA0()  (GameFlowManager::LoadTrack)
    void OnEventPacket(uint32_t eventHash, const char* eventId, bool fromPeer = true) {
        // the sender repeats an invitation for a few seconds (UDP may lose it): act on it once
        if (fromPeer && g_handledHash == eventHash && GetTickCount() - g_handledAt < 30000) return;
        int flow = Game::GameFlowState();
        int mode = 0;
        void* cur = CurrentParms(&mode);
        const char* resultsScreen = FEScreens::IsPushed("PostRace_Results.fng") ? "PostRace_Results.fng"
                                  : FEScreens::IsPushed("InGamePhotoMaster.fng") ? "InGamePhotoMaster.fng" : nullptr;   // winner's photo
        if (cur && mode == 1 && fromPeer && EventHashOf(cur) != eventHash && resultsScreen) {
            void* db0 = nullptr; Mem::Read(0x91E004, db0);
            void* next = db0 ? ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db0, eventHash) : nullptr;
            if (!next || IsChallengeEvent(next)) return;   // one we'd refuse in free roam anyway: stay on the results
            static DWORD lastExit = 0;
            void* fe = nullptr;
            if (GetTickCount() - lastExit > 2500 && Mem::Read(0x91CADC, fe) && fe) {
                lastExit = GetTickCount();
                ((void(__thiscall*)(void*, uint32_t, const char*, uint32_t))0x516C50)(fe, 0x406415E3, resultsScreen, 0xFF);
                LOG("Event: %s invited while we're on the results screen - leaving it to join", eventId);
            }
            return;                               // the invitation is repeated; the next copy finds us in free roam
        }
        if (cur && (mode == 1 || fromPeer)) {     // racing, or already loading into an event
            static DWORD lastLog = 0;
            if (GetTickCount() - lastLog > 5000) { lastLog = GetTickCount(); LOG("Event: %s requested but we're already in an event - ignored", eventId); }
            return;
        }
        void* db = nullptr;
        Mem::Read(0x91E004, db);
        void* parms = db ? ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, eventHash) : nullptr;
        if (!parms) { LOG("Event: unknown event %s (%08X)", eventId, eventHash); return; }
        if (fromPeer && flow == 6 && IsChallengeEvent(parms)) {
            // a Challenge Series event needs its preset car, which only a start from the menu builds
            static DWORD lastLog = 0;
            if (GetTickCount() - lastLog > 10000) { lastLog = GetTickCount(); LOG("Event: %s is a Challenge Series event - join it from the main menu, not from free roam", eventId); }
            return;
        }
        g_joiningEvent = fromPeer ? eventHash : 0;   // a local test start is sent on to the peer
        g_startingEventHash = eventHash;             // lets the car factory know which event is loading
        // build the same AI opponents as the peer (roster packets arrive just before the event)
        g_usePeerRoster = fromPeer && g_peerRosterN > 0 && GetTickCount() - g_peerRosterAt < 5000;
        g_peerRosterNext = 0;
        if (fromPeer) LOG("Event: using %s", g_usePeerRoster ? "the peer's opponents" : "our own opponents (no roster received)");
        // free the vehicle budget so every racer can be built: the traffic mirrors go. Our free-roam copy
        // of the peer's car stays until the race has built their racer: while a car of that model
        // exists the model stays loaded, so the racer can be built as their car. (Removed first, a
        // partner who started far from us often had no model left here: their racer came out wrong or
        // not at all.) The game may still take that car if it needs the room (see Cache_OnQuery).
        {
            const CarInfo* pc = Net::PeerCar();
            if (g_ghost && g_ghostKind == GK_SPAWNED && pc && g_spawnedKey == pc->carKey && !IsChallengeEvent(parms)) {
                g_ghostYieldUntil = GetTickCount() + 15000;
                LOG("Event: keeping our copy of the other player's car until their racer is built");
            } else Game::ReleaseGhost();
        }
        g_racerReplacedThisLoad = false;   // a new race (also when started from free roam: no world load)
        g_replacedRacer = nullptr;
        g_noSpawnUntil = GetTickCount() + 8000;
        for (auto& m : g_mirrors) if (m.used) RemoveMirror(m);
        g_joinStartedAt = GetTickCount();
        if (flow == 6) {
            void* gm = nullptr;
            Mem::Read(0x91E00C, gm);
            if (!gm) { LOG("Event: no GManager"); return; }
            ((void(__thiscall*)(void*, uint32_t))0x5FDCB0)(gm, eventHash);
            sprintf_s(g_eventStatus, "starting %s (free roam)", eventId);
            LOG("Event: starting %s from free roam (GManager 0x5FDCB0)", eventId);
            if (fromPeer) { g_handledHash = eventHash; g_handledAt = GetTickCount(); }   // only once it really started
        } else if (flow == 3) {
            void* c = ((void*(__thiscall*)(void*, void*))0x603440)(db, parms);
            if (!c) { LOG("Event: could not create the race for %s", eventId); return; }
            // challenge events use context 0 (the world load then builds the preset car), career 2
            ((void(__thiscall*)(void*, void*, int))0x600AB0)(db, c, IsChallengeEvent(parms) ? 0 : 2);
            ((void(__thiscall*)(void*, void*))0x5E39E0)(db, c);
            ((void(__cdecl*)())0x57ECA0)();
            sprintf_s(g_eventStatus, "starting %s (menu)", eventId);
            LOG("Event: starting %s from the front end (LoadTrack)", eventId);
            if (fromPeer) { g_handledHash = eventHash; g_handledAt = GetTickCount(); }
        } else {
            LOG("Event: %s requested while loading (flow %d) - ignored", eventId, flow);
            g_joiningEvent = 0;
        }
    }
}

namespace {
    RosterEntry g_inviteRoster[8]; int g_inviteN = 0;
    uint32_t g_inviteHash = 0; char g_inviteId[40] = ""; DWORD g_inviteUntil = 0, g_inviteNext = 0;
    void SendInvite() {
        for (int i = 0; i < g_inviteN; ++i) {
            RosterPacket rp{};
            rp.index = (uint8_t)i; rp.count = (uint8_t)g_inviteN;
            rp.carKey = g_inviteRoster[i].key;
            rp.hasCustom = g_inviteRoster[i].hasCustom ? 1 : 0;
            memcpy(rp.custom, g_inviteRoster[i].custom, sizeof(rp.custom));
            Net::SendRoster(rp);
        }
        Net::SendEvent(g_inviteHash, g_inviteId);
    }
    DWORD g_restartResendAt[2] = { 0, 0 }; uint32_t g_restartResendHash = 0;
    uint32_t g_restartSeq = 0, g_peerRestartSeq = 0; DWORD g_ownRestartAt = 0;
}

static void EventTickImpl(bool connected) {
    int mode = 0, ctx = 0;
    void* parms = CurrentParms(&mode, &ctx);
    if (parms != g_lastParms) {
        g_lastParms = parms;
        const char* id = EventIdOf(parms);
        g_curEventHash = (parms && id) ? EventHashOf(parms) : 0;
        if (!parms) g_handledHash = 0;                         // event over: the same one may be started again
        if (!parms) g_startingEventHash = 0;
        if (g_inviteHash && g_inviteHash != g_curEventHash) g_inviteHash = 0;   // we left it: stop inviting
        if (parms && id) {
            uint32_t h = EventHashOf(parms);
            LOG("Event: race parameters now %s (%08X), mode %d, context %d", id, h, mode, ctx);
            if (h == g_joiningEvent) {
                sprintf_s(g_eventStatus, "joined %s", id);
                g_joiningEvent = 0;                          // we started it because the peer did: don't echo
            } else if (connected && (ctx == 2 /*career*/ || (ctx == 0 && IsChallengeEvent(parms)))) {
                // our AI opponents first, so the peer builds the same ones. Kept and repeated for a
                // few seconds (SendInvite) until the peer is in the race too.
                g_inviteN = g_localRosterN;
                for (int i = 0; i < g_inviteN; ++i) g_inviteRoster[i] = g_localRoster[i];
                g_inviteHash = h; strncpy_s(g_inviteId, id, _TRUNCATE);
                g_inviteUntil = GetTickCount() + 12000; g_inviteNext = GetTickCount() + 1500;   // long enough for a partner to leave a results screen
                SendInvite();
                LOG("Event: sent %d opponent(s) to peer", g_inviteN);
                sprintf_s(g_eventStatus, "started %s - inviting peer", id);
                LOG("Event: sent %s to peer", id);
            }
        }
        g_localRosterN = 0;        // the next event collects its own opponents
        g_usePeerRoster = false;
    }
    // repeat the invitation until the peer is in the race (or 6 s passed)
    if (g_inviteHash) {
        DWORD now = GetTickCount();
        const Net::Snapshot& pr = Net::Latest();
        // a partner still on the results of the previous race reports inRace too: only being in OUR event counts
        if (!connected || (int)(now - g_inviteUntil) > 0 || (pr.valid && pr.s.inRace && pr.s.eventHash == g_inviteHash)) g_inviteHash = 0;
        else if ((int)(now - g_inviteNext) >= 0) { g_inviteNext = now + 1500; SendInvite(); }
    }
    if (g_joiningEvent && GetTickCount() - g_joinStartedAt > 20000) {
        LOG("Event: joining timed out");
        strcpy_s(g_eventStatus, "join timed out");
        g_joiningEvent = 0;
    }
}

bool Game::InstallEventSync() {
    Net::OnRoster = [](const RosterPacket& p) {
        if (!g_coopLinked || p.index >= 8 || p.count > 8) return;
        // a new roster only after a pause: repeats of the same invitation must not reset it mid-build
        if (GetTickCount() - g_peerRosterAt > 3000 && !g_usePeerRoster) g_peerRosterN = 0;
        RosterEntry& r = g_peerRoster[p.index];
        r.key = p.carKey; r.hasCustom = p.hasCustom != 0;
        memcpy(r.custom, p.custom, sizeof(r.custom));
        memcpy(r.name, p.name, sizeof(r.name)); r.name[23] = 0;
        if (p.index + 1 > g_peerRosterN) g_peerRosterN = p.index + 1;
        g_peerRosterAt = GetTickCount();
    };
    Net::OnEvent = [](uint32_t hash, const char* id) {
        if (!g_coopLinked) return;     // one of us is still in the prologue: no shared events
        __try { OnEventPacket(hash, id); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Event: exception handling peer event"); }
    };
    return true;
}

void Game::EventTick(bool connected) {
    if (!g_ok) return;
    __try { EventTickImpl(connected); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Event: exception"); }
}

const char* Game::EventStatus() { return g_eventStatus; }

// Test (F7, TestKeys=1): start an event here exactly as if the peer had started it, but without the
// "don't echo" flag, so once it engages it is also sent to the peer.
static bool DebugStartEventSafe(const char* eventId) {
    void* db = nullptr;
    __try {
        // the test key takes an event ID string: find its race to get the game's event hash
        Mem::Read(0x91E004, db);
        void* parms = db ? ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, HashName(eventId)) : nullptr;
        OnEventPacket(parms ? EventHashOf(parms) : HashName(eventId), eventId, false);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Test command 'events': list the career / challenge events of the race database (IDs "<bin>.<x>.<y>",
// tried for bin 0..20, x 0..9, y 0..15 (0..99 in bin 19) through GRaceDatabase::GetRaceParameters 0x5FB710). Logged per
// event: race type (GRaceParameters 0x5FAA20: 0 sprint, 1 circuit, 2 drag, 3 lap knockout, 4 tollbooth,
// 5 speedtrap, ... 8 pursuit challenge), boss race (0x5FBD90) and the preset car of challenge events
// (0x5FC5C0, attribute 0xC0EEB909 - empty for career events).
static int ListEventsSafe(char* lastLine, size_t n) {
    int found = 0;
    void* db = nullptr;
    __try {
        if (!Mem::Read(0x91E004, db) || !db) return -1;
        char id[16];
        for (int b = 0; b <= 20; ++b) {
            int inBin = 0;
            const int ymax = b == 19 ? 99 : 15;      // bin 19 = Challenge Series, 19.8.1 ...
            for (int x = 0; x <= 9; ++x) for (int y = 0; y <= ymax; ++y) {
                sprintf_s(id, "%d.%d.%d", b, x, y);
                void* p = ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, HashName(id));
                if (!p) continue;
                int type = ((int(__thiscall*)(void*))0x5FAA20)(p);
                bool boss = ((bool(__thiscall*)(void*))0x5FBD90)(p);
                char car[40] = "";
                __try {
                    const uint8_t* a = ((const uint8_t*(__thiscall*)(void*))0x5FC5C0)(p);
                    if (a && Mem::IsReadable(a, 24) && a[0] >= '0' && a[0] <= 'z') sprintf_s(car, " car %.24s", (const char*)a);   // the attribute holds the name inline
                } __except (EXCEPTION_EXECUTE_HANDLER) { car[0] = 0; }
                LOG("Events: %-8s type %d%s%s", id, type, boss ? " BOSS" : "", car);
                ++found; ++inBin;
            }
            if (inBin) sprintf_s(lastLine, n, "bin %d: %d", b, inBin);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
    return found;
}
std::string Game::DebugListEvents() {
    if (!g_ok) return "game not recognised";
    char last[64] = "";
    int n = ListEventsSafe(last, sizeof(last));
    if (n == -1) return "no race database";
    if (n == -2) return "exception (list may be partial)";
    char buf[96]; sprintf_s(buf, "%d events logged (\"Events:\" lines)", n);
    return buf;
}
std::string Game::DebugStartEvent(const char* eventId) {
    if (!g_ok) return "game not recognised";
    if (!DebugStartEventSafe(eventId)) return "exception";
    return std::string("test start of event ") + eventId;
}

// ---------------------------------------------------------------------------
// Co-op rules: no speedbreaker (slowing time can't work when two games share one world).
//  Local IPlayer = first entry of the PLAYER_LIST local bucket (mBegin at 0x92D87C); its concrete vtable
//  is 0x8B0B10: slot 16 InGameBreaker (byte +0x34), 18 ResetGameBreaker(bool full) (false -> meter
//  [+0x38] = 0.0), 20 ToggleGameBreaker.
// ---------------------------------------------------------------------------
static void CoopRulesImpl() {
    void** begin = nullptr;
    if (!Mem::Read(0x92D87C, begin) || !begin) return;
    void* pl = nullptr;
    if (!Mem::Read((uintptr_t)begin, pl) || !pl || VTable(pl) != 0x8B0B10) return;
    bool active = VCall<bool(__thiscall*)(void*)>(pl, 16)(pl);
    if (active) { VCall<void(__thiscall*)(void*)>(pl, 20)(pl); LOG("Co-op: speedbreaker switched off"); }
    float meter = 0;
    if (Mem::Read((uintptr_t)pl + 0x38, meter) && meter > 0.f)
        VCall<void(__thiscall*)(void*, bool)>(pl, 18)(pl, false);
}

// No slow motion in co-op: every slow-motion moment (speed trap flash, pursuit start / crash / finish
// cameras, the speedbreaker, GameSpeed) goes through one time-scale provider, the local player's method 1
// (0x6F4D10, vtable entries 0x8B09A4 and 0x8B0A14). The sim kernel step 0x6F6CF0 multiplies its dt by
// what it returns. While co-op is on we return 1.0: the cameras still play, just in real time.
namespace {
    using TimeScaleFn = float(__thiscall*)(void*, float, float);
    float __fastcall hk_TimeScale(void* self, void*, float dt, float cur) {
        if (g_cfg.noSlowMotion && g_coopLinked && Net::Stats().connected) return 1.0f;
        return ((TimeScaleFn)0x6F4D10)(self, dt, cur);
    }
}
bool Game::InstallNoSlowMotion() {
    int n = 0;
    for (uintptr_t slot : { (uintptr_t)0x8B09A4, (uintptr_t)0x8B0A14 }) {
        uintptr_t cur = 0;
        if (Mem::Read(slot, cur) && cur == 0x6F4D10) { Mem::PatchPointer((void*)slot, (void*)&hk_TimeScale); ++n; }
    }
    LOG("Co-op: real-time (no slow motion) hook %d/2", n);
    return n == 2;
}

void Game::CoopRulesTick(bool connected) {
    if (!g_ok || !connected || !g_cfg.noSpeedbreaker || GameFlowState() != 6) return;
    __try { CoopRulesImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// No pause in co-op: SimKernel_ShouldRun (0x6EF150) stops the simulation while the front end has a
// pause request (pause menu, world map). Skipping that one check (jz -> jmp at 0x6EF1B1) keeps the
// world running while the menu still works. Only while connected; solo play pauses as usual.
// Race restarts: ERestartRace's constructor (0x6307F0) sets "restart pending" (byte 0x91CF28). We watch
// it and tell the peer, which creates the same event (0x627400(8) alloc + ctor) while racing.
// ---------------------------------------------------------------------------
namespace {
    bool  g_noPausePatched = false;
    bool  g_restartFromPeer = false;
    uint8_t g_lastRestartFlag = 0;

    void SetNoPause(bool on) {
        if (on == g_noPausePatched) return;
        const uint8_t jz[2] = { 0x74, 0x11 }, jmp[2] = { 0xEB, 0x11 };
        bool ok = on ? Mem::PatchBytes(0x6EF1B1, jz, jmp, 2) : Mem::PatchBytes(0x6EF1B1, jmp, jz, 2);
        if (ok) g_noPausePatched = on;
        LOG("Coop: pause menu %s%s", on ? "no longer stops the world" : "pauses again", ok ? "" : " (patch FAILED)");
    }

    bool TriggerRestart() {
        void* m = ((void*(__cdecl*)(uint32_t))0x627400)(8);
        if (!m) return false;
        ((void*(__thiscall*)(void*))0x6307F0)(m);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Start together. Each game loads an event at its own speed. CarState::pad[0] carries our stage in the
// current event: 1 = loading / intro cutscene, 2 = on the grid (intro over). The game that gets there
// first freezes its simulation (the front-end pause request counter 0x91CAE4, with the co-op "no
// pause" patch lifted for the moment) until the other one is ready too, or 20 s pass.
// ---------------------------------------------------------------------------
namespace {
    uint8_t  g_stage = 0;
    uint32_t g_stageEvent = 0;
    bool     g_sawNis = false, g_holding = false;
    DWORD    g_modeSince = 0, g_holdSince = 0, g_readyAt = 0;
    char     g_startStatus[64] = "";

    void ReleaseHold(const char* why) {
        if (!g_holding) return;
        g_holding = false;
        int req = 0;
        if (Mem::Read(0x91CAE4, req) && req > 0) { --req; Mem::Write(0x91CAE4, req); }
        g_startStatus[0] = 0;
        LOG("Start: go (%s, waited %.1f s)", why, (GetTickCount() - g_holdSince) / 1000.f);
    }
}

static void StartSyncTickImpl(bool connected) {
    DWORD now = GetTickCount();
    uint32_t hash = g_curEventHash;
    const Net::Snapshot& pr = Net::Latest();
    if (!hash || Game::GameFlowState() != 6) { g_stage = hash ? 1 : 0; g_stageEvent = hash; g_sawNis = false; g_modeSince = 0; ReleaseHold("event over"); return; }
    if (hash != g_stageEvent) { g_stageEvent = hash; g_stage = 1; g_sawNis = false; g_modeSince = 0; g_readyAt = 0; }
    int mode = 0; CurrentParms(&mode);
    bool nis = Game::NisSceneHash() != 0;
    if (nis) g_sawNis = true;
    if (mode == 1) { if (!g_modeSince) g_modeSince = now; } else g_modeSince = 0;
    // on the grid: the intro cutscene is over (or the event has none: 3 s of racing mode without one)
    bool ready = mode == 1 && !nis && g_modeSince && (g_sawNis || now - g_modeSince > 3000);
    if (g_stage == 1 && ready) {
        if (!g_readyAt) g_readyAt = now;
        if ((int)(now - g_readyAt) < g_cfg.readyDelayMs) ready = false;      // test: pretend to load slowly
    }
    if (g_stage == 1 && ready) {
        g_stage = 2;
        // coming = already in this event, or still on the way to it (no event yet and we invited them /
        // they invited us). A peer in ANOTHER event, or roaming and ignoring us, is not waited for.
        // (while our invitation is still being repeated the peer may be leaving the previous race's results)
        bool peerComing = pr.valid && (pr.s.eventHash == hash || g_inviteHash == hash ||
                                       (pr.s.eventHash == 0 && g_handledHash == hash));
        bool peerReady = pr.valid && pr.s.eventHash == hash && pr.s.pad[0] >= 2;
        if (connected && g_cfg.startTogether && peerComing && !peerReady && !g_holding) {
            int req = 0;
            if (Mem::Read(0x91CAE4, req)) { ++req; Mem::Write(0x91CAE4, req); g_holding = true; g_holdSince = now; }
            const std::string& pn = Net::Stats().peerName;
            sprintf_s(g_startStatus, "waiting for %s to load...", pn.empty() ? "the other player" : pn.c_str());
            LOG("Start: on the grid first - %s", g_startStatus);
        }
    }
    if (g_holding) {
        if (!connected || !pr.valid) ReleaseHold("the other player is gone");
        else if (pr.s.eventHash == hash && pr.s.pad[0] >= 2) ReleaseHold("both ready");
        else if (now - g_holdSince > 20000) ReleaseHold("waited long enough");
        else {
            // roaming with no event, or in a different one, for 1.5 s: they are not coming
            static DWORD elsewhereSince = 0;
            bool elsewhere = pr.s.eventHash != hash && pr.s.gameFlowState == 6 && pr.s.hasCar && now - g_holdSince > 3000;
            if (!elsewhere) elsewhereSince = 0; else if (!elsewhereSince) elsewhereSince = now;
            if (elsewhere && now - elsewhereSince > 1500) { elsewhereSince = 0; ReleaseHold("the other player is not joining"); }
        }
    }
}

uint8_t Game::RaceStage() { return g_stage; }
const char* Game::StartStatus() { return g_startStatus; }

static void RestartTickImpl(bool connected) {
    uint8_t flag = 0;
    Mem::Read(0x91CF28, flag);
    if (flag && !g_lastRestartFlag) {
        if (g_restartFromPeer) g_restartFromPeer = false;        // we did it because the peer did
        else if (connected) {
            int mode = 0;
            void* parms = CurrentParms(&mode);
            if (!g_restartSeq) g_restartSeq = GetTickCount();     // differs after a relaunch
            ++g_restartSeq; g_ownRestartAt = GetTickCount();
            Net::SendRestart(EventHashOf(parms), g_restartSeq);
            g_restartResendHash = EventHashOf(parms);            // twice more, in case it was lost
            g_restartResendAt[0] = GetTickCount() + 700; g_restartResendAt[1] = GetTickCount() + 1600;
            LOG("Race: restarted here - telling the peer");
        }
    }
    g_lastRestartFlag = flag;
    for (DWORD& at : g_restartResendAt)
        if (at && (int)(GetTickCount() - at) >= 0) { at = 0; if (connected) Net::SendRestart(g_restartResendHash, g_restartSeq); }
}

void Game::PauseRuleTick(bool connected) {
    if (!g_ok) return;
    __try { if (g_holding && GetTickCount() - g_holdSince > 21000) ReleaseHold("safety limit"); } __except (EXCEPTION_EXECUTE_HANDLER) { g_holding = false; }
    __try {
        StartSyncTickImpl(connected);
        // while we wait on the grid, or a story scene holds / plays over the world, the pause must bite
        SetNoPause(connected && g_cfg.noPause && !g_holding && !Game::StoryPauseWanted());
        RestartTickImpl(connected);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool Game::InstallRestartSync() {
    Net::OnRestart = [](uint32_t hash, uint32_t seq) {
        if (!g_coopLinked) return;
        __try {
            int mode = 0;
            void* parms = CurrentParms(&mode);
            uint8_t pending = 0;
            Mem::Read(0x91CF28, pending);
            if (seq == g_peerRestartSeq) return;                              // a repeat of one we already handled
            if (hash && EventHashOf(parms) != hash) { LOG("Race: peer restarted a different event - ignored"); return; }
            g_peerRestartSeq = seq;
            if (g_ownRestartAt && GetTickCount() - g_ownRestartAt < 2500) return;   // we both restarted at once
            if (Game::GameFlowState() != 6 || mode != 1 || pending) { LOG("Race: peer restarted, but we're not racing - ignored"); return; }
            if (hash && EventHashOf(parms) != hash) { LOG("Race: peer restarted a different event - ignored"); return; }
            g_restartFromPeer = true;
            if (!TriggerRestart()) { g_restartFromPeer = false; LOG("Race: restart failed"); return; }
            LOG("Race: peer restarted the race - restarting here too");
        } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Race: exception restarting"); }
    };
    return true;
}
// Test command: restart the current race here (the restart is then sent to the peer as usual).
static const char* DebugRestartSafe() {
    __try {
        int mode = 0; CurrentParms(&mode);
        if (Game::GameFlowState() != 6 || mode != 1) return "not racing";
        return TriggerRestart() ? "restarting the race" : "restart failed";
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "exception"; }
}
std::string Game::DebugRestart() {
    if (!g_ok) return "game not recognised";
    return DebugRestartSafe();
}
uint32_t Game::CurrentEventHash() { return g_curEventHash; }

// Co-op needs a career that is past the prologue. The prologue is a scripted single-player sequence
// (fixed car, scripted races 16.x and cutscenes); following events or showing cars there would pull
// the other player into it. Profile: FEDatabase *0x91CF90, loaded flag byte +3, user profile +0x10:
// +0xAC bit 0 = career started, +0xB0 == 0x10 = prologue (same checks DirectFreeRoam uses).
static bool CoopAllowedImpl() {
    uint8_t* fedb = nullptr; uint8_t* up = nullptr;
    if (!Mem::Read(0x91CF90, fedb) || !fedb || !fedb[3]) return true;          // no profile loaded yet
    if (!Mem::Read((uintptr_t)fedb + 0x10, up) || !up) return true;
    if (!(up[0xAC] & 1)) return false;                                         // career not started
    if (up[0xB0] == 0x10) return false;                                        // in the prologue
    int mode = 0;
    const char* id = EventIdOf(CurrentParms(&mode));                           // the last race stays set in free roam:
    if (mode == 1 && id && id[0] == '1' && id[1] == '6' && id[2] == '.') return false;   // only racing a prologue event (16.x) counts
    return true;
}
bool Game::CoopAllowed() {
    if (!g_ok) return true;
    __try { return CoopAllowedImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}
void Game::SetCoopLinked(bool linked) { g_coopLinked = linked; }
