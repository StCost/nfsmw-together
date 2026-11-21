// Included from game.cpp (after stream.inl).
// ---------------------------------------------------------------------------
// Fast start
//  [Start] SkipIntro: the boot flow (BootFlowManager, *0x91CF48) takes its screens from static tables of
//    string pointers (0x8F3C58..0x8F3CD0). Pointing the EA logo / PSA / attract movie / "Press START"
//    entries at "" makes the boot go MC_Bootup -> MC_Main (profile autoload) -> MainMenu.
//  [Start] DirectFreeRoam: once the profile is loaded and MainMenu is up, do what Career > Resume
//    Career > Resume Free Roam > Yes do: CareerSettings::ResumeCareer (0x57F570, this = profile+0xA8),
//    GManager+0x110 = 1 (start at the safe house), the career-intro check, 0x56C5B0 (start career free
//    roam), FEGameMode |= 1.
//  [Start] SkipLeaveSafeHouseDialog: CResumeFreeRoam::React (vtable slot 0x89C42C, 0x51FF10) asks
//    "leave the safe house?"; instead send its Yes message (0xD05FC3A3) straight back to the screen.
// ---------------------------------------------------------------------------
namespace {
    using ReactFn = int(__thiscall*)(void*, const char*, uint32_t, void*, uint32_t, uint32_t);
    ReactFn g_origResumeFreeRoamReact = nullptr;
    int __fastcall hk_ResumeFreeRoamReact(void* self, void*, const char* pkg, uint32_t msg, void* obj, uint32_t p1, uint32_t p2) {
        if (msg == 0x0C407210) {   // the option was activated: answer the dialog's Yes ourselves
            void* fe = nullptr;
            if (Mem::Read(0x91CADC, fe) && fe) {
                ((void(__thiscall*)(void*, uint32_t, const char*, uint32_t))0x516C50)(fe, 0xD05FC3A3, pkg, 0xFF);
                LOG("Start: Resume Free Roam without the leave-safe-house dialog");
                return 1;
            }
        }
        return g_origResumeFreeRoamReact(self, pkg, msg, obj, p1, p2);
    }

    bool RaceDone(const char* id) {
        void* db = nullptr;
        if (!Mem::Read(0x91E004, db) || !db) return false;
        uint32_t h = ((uint32_t(__cdecl*)(const char*))0x4519D0)(id);
        void* p = ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, h);
        if (!p) return false;
        uint32_t eh = ((uint32_t(__thiscall*)(void*))0x5FAB40)(p);
        return ((bool(__thiscall*)(void*, uint32_t, int))0x5DCA70)(db, eh, 2);
    }

    // "Press accept" on a front-end screen: a real key when the game is focused, otherwise the FE
    // message the pad/keyboard would produce (PAD_ACCEPT 0x406415E3 via cFEng::QueueGameMessage
    // 0x516C50), so the fast start also works with the game in the background.
    bool PressAccept(const char* pkg) {
        if (Overlay::GameWindow() == GetForegroundWindow()) return Input::Tap(0x1C);
        void* fe = nullptr;
        if (!Mem::Read(0x91CADC, fe) || !fe) return false;
        ((void(__thiscall*)(void*, uint32_t, const char*, uint32_t))0x516C50)(fe, 0x406415E3, pkg, 0xFF);
        return true;
    }
    // The memory-card screens (MC_Main.fng: boot autoload, save/load prompts) get their messages
    // through NotificationMessage, thiscall(msg, object, param1, param2). Two screen classes handle
    // MC_Main (vtable slots 0x89EDA4 -> 0x5644F0 and 0x89EDD4 -> 0x564630; both end in the shared
    // 0x564070). We watch both to know the live screen object and, when the game has no focus, hand
    // it the messages Enter on the OK button (name hash 0xB8A7C6CD) produces - see McNotify.
    enum class Boot { Waiting, Settling, Done } g_boot = Boot::Waiting;
    DWORD g_bootReadyAt = 0;

    using NotifyFn = void(__thiscall*)(void*, uint32_t, void*, uint32_t, uint32_t);
    NotifyFn g_origMcNotify[2] = {};
    void*    g_mcScreen = nullptr;          // live MC_Main screen object
    int      g_mcScreenKind = -1;
    DWORD    g_mcScreenSeen = 0;

    // The memory card operation the MC_Main prompt is about (*0x91CBAC & 0x0F000000). We only ever
    // answer 0 (boot autoload / "load complete") and 8 / 9 (autosave notices); never a question.
    bool McOpHarmless(uint32_t* opOut) {
        uint32_t mc = 0;
        if (!Mem::Read(0x91CBAC, mc)) return false;
        uint32_t op = mc & 0x0F000000;
        if (opOut) *opOut = op;
        return op == 0 || op == 0x08000000 || op == 0x09000000;
    }
    uint8_t* McOkButton() {     // the OK button of MC_Main.fng (package +0x8C, name hash 0xB8A7C6CD)
        void* cfeng = nullptr; uint8_t* btn = nullptr; uint32_t hash = 0;
        uint8_t* pkg = (Mem::Read(0x91CADC, cfeng) && cfeng) ? ((uint8_t*(__thiscall*)(void*, const char*))0x516AF0)(cfeng, "MC_Main.fng") : nullptr;
        if (!pkg || !Mem::Read((uintptr_t)pkg + 0x8C, btn) || !btn || !Mem::Read((uintptr_t)btn + 0x10, hash) || hash != 0xB8A7C6CD) return nullptr;
        return btn;
    }
    // A requested "OK" for one prompt: delivered only to that screen object and button, for the same
    // operation, while the game is still unfocused, and only within ~1 s of the request.
    struct McConfirm { void* self; uint8_t* btn; uint32_t op; DWORD tick; int step; DWORD stepAt; };
    McConfirm g_mcReq = {};

    // Enter on the OK button reaches the screen as two messages: 0xC502DF5D (button pressed - starts the
    // pending memory-card step, e.g. the profile load) and, on release, 0x0C407210 (button activated -
    // closes the prompt). We send the same pair, the second ~150 ms later. p2 is the same value every
    // message to this screen carries (a real press has it too), so the incoming message's p2 is used.
    // Called BEFORE the incoming message is forwarded, so the screen object is certainly alive.
    void McDeliver(int kind, void* self, uint32_t p2) {
        McConfirm& r = g_mcReq;
        if (!r.step) return;
        DWORD now = GetTickCount();
        bool stale = r.self != self || (r.step == 1 && now - r.tick > 1000) || (r.step == 2 && now - r.stepAt > 1000)
                     || Overlay::GameWindow() == GetForegroundWindow();
        uint32_t op = 0;
        if (!stale) stale = McOkButton() != r.btn || !McOpHarmless(&op) || op != r.op;
        if (stale) { r = {}; return; }
        if (r.step == 2 && now - r.stepAt < 150) return;
        __try {
            if (r.step == 1) {
                r.step = 2; r.stepAt = now;
                LOG("Start: OK on the load screen (no focus)");
                g_origMcNotify[kind](self, 0xC502DF5D, r.btn, 0, p2);
            } else {
                uint8_t* btn = r.btn; r = {};
                g_origMcNotify[kind](self, 0x0C407210, btn, 0, p2);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { r = {}; LOG("Start: exception confirming the load screen"); }
    }
    void McNotify(int kind, void* self, uint32_t msg, void* obj, uint32_t p1, uint32_t p2) {
        if (g_boot != Boot::Done) {      // after the boot: pure pass-through
            if (self != g_mcScreen) g_mcReq = {};
            g_mcScreen = self; g_mcScreenKind = kind; g_mcScreenSeen = GetTickCount();
            if (g_cfg.bootTrace) {
                uint32_t h = 0; if (obj && Mem::IsReadable((uint8_t*)obj + 0x10, 4)) Mem::Read((uintptr_t)obj + 0x10, h);
                static uint32_t lastMsg = 0; static int repeats = 0;
                if (msg != lastMsg) { lastMsg = msg; repeats = 0; }
                if (++repeats <= 2) LOG("BootTrace: MC_Main[%d] notify %08X obj %p (name %08X) p1 %08X p2 %08X", kind, msg, obj, h, p1, p2);
            }
            if (msg == 0xC502DF5D || msg == 0x0C407210) g_mcReq = {};      // a real key press: ours is not needed
            else McDeliver(kind, self, p2);
        }
        g_origMcNotify[kind](self, msg, obj, p1, p2);
    }
    void __fastcall hk_McNotify0(void* self, void*, uint32_t msg, void* obj, uint32_t p1, uint32_t p2) { McNotify(0, self, msg, obj, p1, p2); }
    void __fastcall hk_McNotify1(void* self, void*, uint32_t msg, void* obj, uint32_t p1, uint32_t p2) { McNotify(1, self, msg, obj, p1, p2); }
    // The boot's profile screen (MC_Main) waits for accept before it loads the profile (memory card
    // state *0x91CBAC = 0x20, pending step 5 at card+0x40) - focused or not. Focused we press Enter.
    // Without focus the pad messages don't reach it, so McNotify delivers the button messages itself.
    // Only for harmless prompts (operation code 0 / 8 / 9), once per prompt - focused or not (a new
    // player without a profile gets real questions here, which are never answered for them).
    // Is there a saved profile at all (Documents\NFS Most Wanted\<name>\<name>)? Without one the boot
    // creates a profile ("Create a new alias?", name entry, autosave question) - never answered for the
    // player: once the alias question is answered, those prompts show operation code 0 too.
    bool AnyProfileSaved() {
        char docs[MAX_PATH] = "";
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) return true;
        std::string root = std::string(docs) + "\\NFS Most Wanted\\";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((root + "*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool found = false;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            DWORD a = GetFileAttributesA((root + fd.cFileName + "\\" + fd.cFileName).c_str());
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) found = true;
        } while (!found && FindNextFileA(h, &fd));
        FindClose(h);
        return found;
    }
    // Several profiles: after "Press START" the boot shows a "load" list of them (MC_List.fng) with the
    // first one highlighted; nothing loads until one is picked. The list's entries come from the
    // memory card's file list (circular list at [[0x91CB20]+0xCC]+0x7C; each node's name at node+8,
    // after a prefix as long as the string 0x6CBE20([0x91CB20]+0xD0) returns) in list order, filtered
    // by Profiles::Listed - all read the same way by the list setup at 0x563A26.
    // With DirectFreeRoam the profile loaded last ([Start] LastProfile, written below) is picked by
    // itself: PAD_DOWN to its row, then PAD_ACCEPT. A name that isn't in the list: left to the player.
    std::vector<std::string> SavedProfileNames() {                // Documents\NFS Most Wanted\<name>\<name>
        std::vector<std::string> out;
        char docs[MAX_PATH] = "";
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) return out;
        std::string root = std::string(docs) + "\\NFS Most Wanted\\";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((root + "*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            DWORD a = GetFileAttributesA((root + fd.cFileName + "\\" + fd.cFileName).c_str());
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return out;
    }
    // the game's list entries, raw (filled into a fixed table so this can run under SEH)
    int GameProfileListRaw(char (*names)[16], int max) {
        int n = 0;
        __try {
            uint8_t* mc = nullptr; uint8_t* card = nullptr; void* rdr = nullptr;
            if (!Mem::Read(0x91CB20, mc) || !mc || !Mem::Read((uintptr_t)mc + 0xCC, card) || !card || !Mem::Read((uintptr_t)mc + 0xD0, rdr) || !rdr) return 0;
            const char* pre = ((const char*(__thiscall*)(void*))0x6CBE20)(rdr);
            size_t pl = pre ? strlen(pre) : 0;
            uint8_t* head = card + 0x7C; uint8_t* node = nullptr;
            Mem::Read((uintptr_t)head, node);
            for (int guard = 0; node && node != head && guard < 64 && n < max; ++guard) {
                const char* nm = (const char*)node + 8 + pl;
                size_t l = strnlen(nm, 15);
                if (l < 15) { memcpy(names[n], nm, l + 1); if (Profiles::Listed(names[n])) ++n; }
                if (!Mem::Read((uintptr_t)node, node)) break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return n;
    }
    std::vector<std::string> GameProfileList() {
        char names[32][16] = {};
        int n = GameProfileListRaw(names, 32);
        return std::vector<std::string>(names, names + n);
    }
    void QueueFeMessage(uint32_t msg, const char* pkg) {
        void* fe = nullptr;
        if (Mem::Read(0x91CADC, fe) && fe) ((void(__thiscall*)(void*, uint32_t, const char*, uint32_t))0x516C50)(fe, msg, pkg, 0xFF);
    }
    // MC_List up: walk to the remembered profile and pick it (one step per call, ~4 per second)
    bool PickProfileStep(void* fe) {
        static int state = 0, target = -1, sent = 0; static DWORD since = 0, last = 0;
        if (state == 3) return false;                                  // done (or left to the player)
        bool up = ((bool(__thiscall*)(void*, const char*))0x516B50)(fe, "MC_List.fng");
        if (!up) { since = 0; return false; }
        if (!since) since = GetTickCount();
        if (state == 0) {
            if (GetTickCount() - since < 800) return true;            // let the list fill in
            std::vector<std::string> list = GameProfileList();
            std::string names;
            for (auto& s : list) names += (names.empty() ? "" : ", ") + s;
            target = Profiles::IndexOf(list, g_cfg.lastProfile);
            if (!g_cfg.directFreeRoam || target < 0) {
                LOG("Start: profile list [%s] - %s", names.c_str(), !g_cfg.directFreeRoam ? "left to the player (quick start off)"
                    : g_cfg.lastProfile.empty() ? "no profile remembered yet, pick one" : "the remembered profile isn't in it, pick one");
                state = 3; return true;
            }
            LOG("Start: profile list [%s] - picking %s (row %d)", names.c_str(), g_cfg.lastProfile.c_str(), target + 1);
            state = 1; sent = 0; last = GetTickCount();
            return true;
        }
        if (GetTickCount() - last < 250) return true;
        last = GetTickCount();
        if (state == 1 && sent < target) { QueueFeMessage(0x911C0A4B /*PAD_DOWN*/, "MC_List.fng"); ++sent; return true; }
        if (state == 1) { state = 2; return true; }                     // one more pause after the last step
        QueueFeMessage(0x406415E3 /*PAD_ACCEPT*/, "MC_List.fng");
        state = 3;
        return true;
    }
    // Once a profile is loaded: remember which one ([Start] LastProfile) for the next start.
    void RememberLoadedProfile(uint8_t* fedb) {
        static bool done = false;
        if (done) return;
        uint8_t* up = nullptr;
        if (!Mem::Read((uintptr_t)fedb + 0x10, up) || !up) return;
        done = true;
        static uint8_t block[0x1000];
        size_t got = 0;
        while (got < sizeof(block) && Mem::SafeRead(up + got, block + got, 0x100)) got += 0x100;
        std::vector<std::string> names = SavedProfileNames();
        std::string alias = Profiles::FindAlias(block, got, names);
        if (alias.empty()) { LOG("Start: loaded profile not recognised (%d saved) - not remembered", (int)names.size()); return; }
        if (_stricmp(alias.c_str(), g_cfg.lastProfile.c_str()) == 0) return;
        g_cfg.lastProfile = alias;
        if (!g_cfg.iniFile.empty()) WritePrivateProfileStringA("Start", "LastProfile", alias.c_str(), g_cfg.iniFile.c_str());
        LOG("Start: remembered profile %s for the next start", alias.c_str());
    }
    bool g_mcSawQuestion = false;       // a real question was on screen during this boot: answer nothing more
    bool ConfirmLoadScreen(DWORD shownMs) {
        void* cfeng = nullptr;
        if (shownMs < 500 || !Mem::Read(0x91CADC, cfeng) || !cfeng) return false;
        static int profileSaved = -1;
        if (profileSaved < 0) {
            profileSaved = AnyProfileSaved() ? 1 : 0;
            if (!profileSaved) LOG("Start: no saved profile - the profile screens are left to the player");
        }
        if (!profileSaved || g_mcSawQuestion) return false;
        uint8_t* btn = McOkButton();
        uint32_t op = 0;
        if (!btn) return false;
        if (!McOpHarmless(&op)) {                                              // never answer other questions
            LOG("Start: the profile screen asks a question (op %08X) - left to the player", op);
            g_mcSawQuestion = true;
            return false;
        }
        static uint8_t* lastBtn = nullptr; static uint32_t lastOp = 0xFFFFFFFF; static DWORD seenAt = 0, sentAt = 0;
        if (btn != lastBtn || op != lastOp) { lastBtn = btn; lastOp = op; seenAt = GetTickCount(); sentAt = 0; return false; }
        if (GetTickCount() - seenAt < 600) return false;                       // let the box finish appearing
        if (sentAt && GetTickCount() - sentAt < 5000) return false;            // once; again only if it's still there after 5 s
        if (Overlay::GameWindow() == GetForegroundWindow()) { sentAt = GetTickCount(); return Input::Tap(0x1C); }
        if (g_mcScreenKind < 0 || !g_origMcNotify[g_mcScreenKind] || !g_mcScreen || GetTickCount() - g_mcScreenSeen > 1000) return false;
        sentAt = GetTickCount();
        g_mcReq = { g_mcScreen, btn, op, GetTickCount(), 1, 0 };             // McNotify delivers it with the screen's next message
        return true;
    }

    // returns a message for the log
    const char* DirectFreeRoamNow() {
        uint8_t* fedb = nullptr;
        Mem::Read(0x91CF90, fedb);
        uint8_t* up = nullptr;
        if (!fedb || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up) return "no profile";
        if (!(up[0xAC] & 1)) return "career not started in this profile";
        bool prologue = up[0xB0] == 0x10;
        bool special = !((*(uint32_t*)(up + 0xAC) >> 14) & 1) && RaceDone("1.2.3");
        ((void(__thiscall*)(void*))0x57F570)(up + 0xA8);                  // ResumeCareer
        if (prologue || special) return "Resume Career started loading by itself";
        uint8_t* gm = nullptr;
        if (Mem::Read(0x91E00C, gm) && gm) gm[0x110] = 1;                 // start from the safe house
        uint8_t debugFlag = 0; Mem::Read(0x926126, debugFlag);
        if (!debugFlag && !RaceDone("16.2.1")) {                          // same as the safe house Yes handler
            void* db = nullptr; Mem::Read(0x91E004, db);
            const char* name = ((const char*(__thiscall*)(void*))0x600C30)(db);
            if (name) {
                uint32_t h = ((uint32_t(__cdecl*)(const char*))0x4519D0)(name);
                void* p = ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, h);
                if (p) {
                    void* c = ((void*(__thiscall*)(void*, void*))0x603440)(db, p);
                    ((void(__thiscall*)(void*, void*, int))0x600AB0)(db, c, 2);
                    ((void(__thiscall*)(void*, void*))0x5E39E0)(db, c);
                }
            }
        }
        ((void(__cdecl*)())0x56C5B0)();                                   // start career free roam
        *(uint32_t*)(fedb + 0x12C) |= 1;
        return "loading career free roam";
    }
}

void Game::InstallBootPatches() {
    if (g_cfg.skipIntro) {
        int n = 0;
        static const char empty[] = "";
        // (the "Press START" splash stays: pressing START there sets up the active controller; we press it)
        static const char* drop[] = { "LS_EAlogo.fng", "LS_PSA.fng", "MW_LS_AttractFMV.fng" };
        for (uintptr_t a = 0x8F3C58; a < 0x8F3CD0; a += 4) {
            const char* s = nullptr;
            if (!Mem::Read(a, s) || !s || !Mem::IsReadable(s, 4)) continue;
            for (const char* d : drop)
                if (!_stricmp(s, d)) { Mem::PatchPointer((void*)a, (void*)empty); ++n; break; }
        }
        LOG("Start: skip intro screens - %d boot entries removed", n);
    }
    // MC_Main (profile load screen) NotificationMessage - see McNotify. Only needed by the fast start;
    // the slot check (it must still hold v1.3's function) is the version check.
    if (g_cfg.skipLoadComplete || g_cfg.directFreeRoam || g_cfg.startInBackground) {
        static const struct { uintptr_t slot, fn; void* hook; } mc[2] = {
            { 0x89EDA4, 0x5644F0, (void*)&hk_McNotify0 }, { 0x89EDD4, 0x564630, (void*)&hk_McNotify1 } };
        for (int i = 0; i < 2; ++i) {
            uintptr_t cur = 0;
            if (Mem::Read(mc[i].slot, cur) && cur == mc[i].fn) {
                g_origMcNotify[i] = (NotifyFn)cur;
                Mem::PatchPointer((void*)mc[i].slot, mc[i].hook);
            } else LOG("Start: MC_Main notify slot %X holds %08X - not watched", (unsigned)mc[i].slot, (unsigned)cur);
        }
    }
    if (g_cfg.skipLeaveSafeHouseDialog) {
        uintptr_t cur = 0;
        if (Mem::Read(0x89C42C, cur) && cur == 0x51FF10) {
            g_origResumeFreeRoamReact = (ReactFn)cur;
            Mem::PatchPointer((void*)0x89C42C, (void*)&hk_ResumeFreeRoamReact);
            LOG("Start: no leave-safe-house dialog");
        } else LOG("Start: Resume Free Roam slot holds %08X - dialog kept", (unsigned)cur);
    }
}

bool Game::DirectStartActive() { return g_cfg.directFreeRoam && g_boot != Boot::Done; }

void Game::BootTick() {
    if (!g_ok || g_boot == Boot::Done || !(g_cfg.directFreeRoam || g_cfg.skipLoadComplete)) return;
    __try {
        if (g_cfg.bootTrace && GameFlowState() == 3) {      // [Debug] BootTrace: what the memory card / pad state does
            static uint32_t last[8] = { 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD };
            uint32_t now[8] = {};
            uint8_t* mcObj = nullptr; Mem::Read(0x91CB20, mcObj);
            Mem::Read(0x91CBAC, now[0]); Mem::Read(0x91CBD8, now[1]); Mem::Read(0x982BEC, now[2]);
            Mem::Read(0x91CBC8, now[3]); Mem::Read(0x91CBCC, now[4]);
            if (mcObj) { Mem::Read((uintptr_t)mcObj + 0x40, now[5]); Mem::Read((uintptr_t)mcObj + 0x18, now[6]); Mem::Read((uintptr_t)mcObj + 0x24, now[7]); }
            if (memcmp(last, now, sizeof(now))) {
                memcpy(last, now, sizeof(now));
                LOG("BootTrace: mc %08X pad %08X active %u prev %08X op %08X | card+40 %08X +18 %08X +24 %08X fg %d",
                    now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], Overlay::GameWindow() == GetForegroundWindow());
            }
        }
        if (GameFlowState() != 3) { if (GameFlowState() == 6) g_boot = Boot::Done; return; }
        uint8_t* fedb = nullptr; void* bfm = (void*)1; void* fe = nullptr;
        Mem::Read(0x91CF90, fedb); Mem::Read(0x91CF48, bfm); Mem::Read(0x91CADC, fe);
        // "Press START" (splash) and "load complete" (MC_Main) wait for a key: press it. The title screen
        // is WS_MW_LS_Splash.fng with a widescreen fix installed and MW_LS_Splash.fng in the plain game.
        const char* splash = nullptr;
        if (g_cfg.directFreeRoam && fe && bfm) {
            for (const char* s : { "WS_MW_LS_Splash.fng", "MW_LS_Splash.fng" })
                if (((bool(__thiscall*)(void*, const char*))0x516B50)(fe, s)) { splash = s; break; }
        }
        if (splash) {
            static DWORD since = 0, lastTap = 0;
            if (SaveSyncHoldsBoot()) return;      // the host's copy of our save may still arrive
            if (!since) since = GetTickCount();
            if (GetTickCount() - since > 1500 && GetTickCount() - lastTap > 2500) {
                lastTap = GetTickCount();
                if (PressAccept(splash)) LOG("Start: pressing START on the title screen (%s)", splash);
            }
            return;
        }
        if (fe && bfm && PickProfileStep(fe)) return;                     // several profiles: the list
        if (fedb && fedb[3]) RememberLoadedProfile(fedb);
        if (fe && bfm && ((bool(__thiscall*)(void*, const char*))0x516B50)(fe, "MC_Main.fng")) {
            static DWORD since = 0, lastTap = 0;
            if (!since) since = GetTickCount();
            // [Start] SkipLoadComplete: confirm it right away (retry until the screen accepts it)
            if (GetTickCount() - since > 300 && GetTickCount() - lastTap > 1000) {
                lastTap = GetTickCount();
                if (ConfirmLoadScreen(GetTickCount() - since)) LOG("Start: confirming the profile load screen");
            }
            return;
        }
        if (!g_cfg.directFreeRoam) { if (!bfm) g_boot = Boot::Done; return; }   // only skipping the load screen
        bool ready = fedb && fedb[3] /*profile loaded*/ && !bfm && fe &&
                     ((bool(__thiscall*)(void*, const char*))0x516B50)(fe, "MainMenu.fng");
        if (!ready) { g_boot = Boot::Waiting; return; }
        if (g_boot == Boot::Waiting) { g_boot = Boot::Settling; g_bootReadyAt = GetTickCount(); return; }
        if (GetTickCount() - g_bootReadyAt < 1000) return;   // let the garage finish loading
        g_boot = Boot::Done;
        LOG("Start: direct free roam - %s", DirectFreeRoamNow());
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_boot = Boot::Done; LOG("Start: exception in direct free roam"); }
}

// [UI] SkipPursuitResults: after a pursuit in free roam the game shows a result screen and waits for a
// key: PostRace_MilestoneRewards.fng ("Bounty achieved", after an escape - seen on the laptop) or
// PostRace_Pursuit.fng. Close it by itself (only this player's game). The busted screen is different
// (GarageMain "Payment for infractions" - costs money) and is left alone.
void Game::PursuitResultTick() {
    if (!g_ok || !g_cfg.skipPursuitResults || GameFlowState() != 6) return;
    static DWORD since = 0, lastTap = 0;
    static const char* const kResult[] = { "PostRace_MilestoneRewards.fng", "PostRace_Pursuit.fng" };
    const char* open = nullptr;
    for (const char* p : kResult) if (FEScreens::IsPushed(p)) { open = p; break; }
    int mode = 0; void* rs = nullptr;
    if (Mem::Read(0x91E000, rs) && rs) Mem::Read((uintptr_t)rs + 0x1960, mode);
    if (!open || mode == 1) { since = 0; return; }        // not open, or it belongs to a race
    if (!since) since = GetTickCount();
    if (GetTickCount() - since > 700 && GetTickCount() - lastTap > 1500) {
        lastTap = GetTickCount();
        // Always the FE message addressed to that package (also when focused): a real Enter would go to
        // whatever dialog is on top.
        void* fe = nullptr;
        if (Mem::Read(0x91CADC, fe) && fe) {
            ((void(__thiscall*)(void*, uint32_t, const char*, uint32_t))0x516C50)(fe, 0x406415E3, open, 0xFF);
            LOG("UI: closing the pursuit result screen (%s)", open);
        }
    }
}
std::string Game::FEAccept(const char* pkg) {
    void* fe = nullptr;
    if (!g_ok || !Mem::Read(0x91CADC, fe) || !fe) return "no front end";
    ((void(__thiscall*)(void*, uint32_t, const char*, uint32_t))0x516C50)(fe, 0x406415E3, pkg, 0xFF);
    return std::string("accept sent to ") + pkg;
}

// Notices in the game's own UI: the EA Trax chyron (the banner that shows the song title when a track
// starts). The game summons it with 0x595ED0, cdecl(title, artist, album): it stores the three string
// pointers (0x8F3C48 / 4C / 50; the screen reads them when it appears) and pushes "EA_Trax.fng" - or
// replays it if it's already up. It does nothing while *0x8EE348 > 0 (chyrons suppressed, e.g. during
// a cutscene). The strings must stay valid, so they live in static buffers. Lines 1 and 2 are shown.
//
// How long it stays: not a C++ timer. EA_TRAX.fng (GLOBAL\GLOBALB.BUN) has an invisible string object
// "SEC" (hash 0x545570C6) whose script TIMER (id 0x03C43A20, 6000 FE time units) chains to SEND
// (0x001C9AC9, 1620 units); SEND's events make the banner slide out and then HIDE it. FE time runs at
// 1200 units/s (x2 / x3 with [UI] MenuSpeed), so the banner is gone after ~6.4 s, ~3.2 s at MenuSpeed 2.
// FEScript (from the FNG loader 0x5C3610 and the script update 0x5C19D0): +0x0C length, +0x10 current
// time (int; finished at length + 1), +0x18 chain-to id, +0x30 id. FEObject +0x54 = current script.
// To keep our notice up for [UI] NoticeSeconds, the TIMER's time is held short of its end until then;
// after that the game's own SEND / slide-out runs as usual. If the object or script isn't found, the
// banner behaves as before.
namespace {
    DWORD g_chyronHoldUntil = 0;     // 0 = not holding
    bool  g_chyronHeld = false;      // the hold engaged at least once for this notice

    void ChyronHoldStart() {
        int speed = (g_cfg.uiMenuSpeed == 2 || g_cfg.uiMenuSpeed == 3) ? g_cfg.uiMenuSpeed : 1;
        // after the release, the rest of TIMER (600 units) runs before the slide-out starts
        DWORD tail = 600u * 1000u / (1200u * (DWORD)speed);
        DWORD total = (DWORD)g_cfg.noticeSeconds * 1000u;
        g_chyronHoldUntil = (GetTickCount() + (total > tail ? total - tail : 0)) | 1;
        g_chyronHeld = false;
    }

    // 1 = held / running normally, 0 = object not found, -1 = exception
    int ChyronHoldRaw() {
        __try {
            uint8_t* obj = ((uint8_t*(__cdecl*)(const char*, uint32_t))0x5251E0)((const char*)0x897414, 0x545570C6);
            if (!obj) return 0;
            uint8_t* scr = *(uint8_t**)(obj + 0x54);
            if (!scr || *(uint32_t*)(scr + 0x30) != 0x03C43A20) return 1;       // not in TIMER (yet / any more)
            uint32_t len = *(uint32_t*)(scr + 0x0C);
            uint32_t hold = len > 1200 ? len - 600 : len / 2;
            if (*(uint32_t*)(scr + 0x10) > hold) {
                *(uint32_t*)(scr + 0x10) = hold;
                if (!g_chyronHeld) { g_chyronHeld = true; LOG("Notice: holding the game banner for %d s", g_cfg.noticeSeconds); }
            }
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }
}

void Game::ChyronTick() {
    if (!g_chyronHoldUntil) return;
    if ((int)(GetTickCount() - g_chyronHoldUntil) >= 0) {
        g_chyronHoldUntil = 0;
        if (!g_chyronHeld) LOG("Notice: the game banner was never held (it ended by itself or was not found)");
        return;
    }
    int r = ChyronHoldRaw();
    // the package may appear a frame or two after it was summoned: give it a moment before giving up
    if (r < 0 || (r == 0 && g_cfg.noticeSeconds * 1000 - (int)(g_chyronHoldUntil - GetTickCount()) > 1500)) {
        LOG("Notice: %s - the banner keeps the game's own timing", r < 0 ? "exception while holding the game banner" : "banner timer object not found");
        g_chyronHoldUntil = 0;
    }
}

// ---------------------------------------------------------------------------
// Notices in the centre of the screen, coloured by importance: the HUD's generic message - the line
// the race script uses for its hints and the race shows split times in (green ahead / orange behind).
//  The first player's HUD ([0x92D87C][0] -> slot 8 -> +4) gives interface 0x5650B0 (UCOM find 0x5D59F0);
//  its slot 1 (0x568030 in the HUD) shows (text, 0, colour hash, 0, 0, priority) and returns false while
//  a message with a higher priority is up (HUD+0x14 = current priority). The text is copied, 64 bytes
//  (so 63 characters: longer notices go in parts, noticeutil.h). Colour hashes from the split times
//  (0x57AC52 / 0x57AC7D): 0xA19BB14C green, 0x5230FAF6 orange. Script messages use priority 5, split
//  times 3; ours use 4. Where there's no HUD (menus) or it's hidden (a cutscene), the notice goes to the
//  game's song banner (ShowChyron below) instead, which the game shows there.
// ---------------------------------------------------------------------------
//  The "colour" is really the show animation (FEScript id) run on the message's text object (gm+8);
//  the HUD's update (0x567F80) clears the message (priority back to 0) once that script has ended. It
//  lasts ~1.3 s at [UI] MenuSpeed 2, so - like the song banner - we hold the script at its middle (fully
//  visible) for the notice's time, then let it run out.
namespace {
    constexpr uint32_t HudGreen = 0xA19BB14C, HudOrange = 0x5230FAF6;
    constexpr int NoticePriority = 4;
    char g_hudText[64];
    DWORD g_hudHoldUntil = 0; uint32_t g_hudHoldScript = 0;
    void* HudMessageIface() {     // null where there's no HUD
        int cnt = 0; void** arr = nullptr;
        if (!Mem::Read(0x92D884, cnt) || cnt <= 0 || !Mem::Read(0x92D87C, arr) || !arr || !arr[0]) return nullptr;
        void* p = arr[0];
        void* x = VCall<void*(__thiscall*)(void*)>(p, 8)(p);
        void* obj = x ? *(void**)((char*)x + 4) : nullptr;
        return obj ? ((void*(__thiscall*)(void*, uint32_t))0x5D59F0)(obj, 0x5650B0) : nullptr;
    }
    // 1 shown, 0 a more important message is up (try again soon), -1 no HUD here
    int HudMessageRaw(const char* text, bool warn) {
        __try {
            void* gm = HudMessageIface();
            if (!gm) return -1;
            strncpy_s(g_hudText, text, _TRUNCATE);
            uint32_t script = warn ? HudOrange : HudGreen;
            bool ok = VCall<bool(__thiscall*)(void*, const char*, int, uint32_t, int, int, int)>(gm, 1)
                (gm, g_hudText, 0, script, 0, 0, NoticePriority);
            if (ok) g_hudHoldScript = script;
            return ok ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    // every frame while a notice should stay: keep its show script at the middle
    void HudHoldRaw() {
        __try {
            uint8_t* gm = (uint8_t*)HudMessageIface();
            uint8_t* txt = gm ? *(uint8_t**)(gm + 8) : nullptr;
            uint8_t* scr = txt ? *(uint8_t**)(txt + 0x54) : nullptr;
            if (!scr || *(uint32_t*)(scr + 0x30) != g_hudHoldScript) return;
            uint32_t len = *(uint32_t*)(scr + 0x0C), hold = len / 2;
            if (*(uint32_t*)(scr + 0x10) > hold) *(uint32_t*)(scr + 0x10) = hold;
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_hudHoldUntil = 0; }
    }
    struct QueuedNotice { std::string full; std::vector<std::string> parts; size_t next; bool warn; DWORD at; };
    std::deque<QueuedNotice> g_notices;
    DWORD g_noticeNext = 0;
}
void Game::QueueNotice(const std::string& text, bool warn) {
    if (text.empty()) return;
    g_notices.push_back({ text, Notice::Split(text), 0, warn, GetTickCount() });
    while (g_notices.size() > 6) g_notices.pop_front();             // a burst: the newest ones matter
}
void Game::NoticeTick() {
    if (!g_ok) return;
    DWORD now = GetTickCount();
    if (g_hudHoldUntil) { if ((int)(now - g_hudHoldUntil) >= 0) g_hudHoldUntil = 0; else HudHoldRaw(); }
    if (g_notices.empty()) return;
    if (!g_cfg.gameNotices) { g_notices.clear(); return; }
    if ((int)(now - g_noticeNext) < 0) return;
    QueuedNotice& q = g_notices.front();
    if (now - q.at > 20000) { LOG("Notice: not shown in 20 s, dropped: %s", q.full.c_str()); g_notices.pop_front(); return; }
    int r = (GameFlowState() == 6 && !NisActive()) ? HudMessageRaw(q.parts[q.next].c_str(), q.warn) : -1;
    if (r == 1) {
        // [UI] NoticeSeconds for the whole notice, shared by its parts (at least 3 s each)
        DWORD partMs = (DWORD)g_cfg.noticeSeconds * 1000u / (DWORD)q.parts.size();
        if (partMs < 3000) partMs = 3000;
        g_hudHoldUntil = (now + partMs) | 1;
        if (++q.next >= q.parts.size()) g_notices.pop_front();
        g_noticeNext = now + partMs;
    } else if (r == 0) {
        g_noticeNext = now + 300;                                       // a race message is up: right after it
    } else if (ShowChyron("Most Wanted Together", q.full.c_str(), "")) {   // no HUD here: the song banner
        LOG("Notice: in the game banner (no HUD here): %s", q.full.c_str());
        g_notices.pop_front();
        g_noticeNext = now + 1000;
    } else g_noticeNext = now + 500;
}

bool Game::ShowChyron(const char* title, const char* line2, const char* line3) {
    if (!g_ok) return false;
    int flow = GameFlowState();
    if (flow != 6 && flow != 3) return false;                 // only where the game shows its own chyron
    void* bootFlow = nullptr;
    if (flow == 3 && (!Mem::Read(0x91CF48, bootFlow) || bootFlow)) return false;   // not over the boot screens
    static char t[3][64];
    __try {
        int blocked = 0; void* fe = nullptr;
        if (!Mem::Read(0x8EE348, blocked) || !Mem::Read(0x91CADC, fe) || !fe) return false;
        // during a cutscene the game holds its own banners back (counter 0x8EE348 > 0); ours (e.g. the skip
        // vote) are wanted there too, so lift the hold for this one call
        bool nis = NisActive();
        if (blocked > 0 && !nis) return false;
        int zero = 0;
        if (blocked > 0) Mem::Write(0x8EE348, zero);
        strncpy_s(t[0], title ? title : "", _TRUNCATE);
        strncpy_s(t[1], line2 ? line2 : "", _TRUNCATE);
        strncpy_s(t[2], line3 ? line3 : "", _TRUNCATE);
        ((void(__cdecl*)(const char*, const char*, const char*))0x595ED0)(t[0], t[1], t[2]);
        if (blocked > 0) Mem::Write(0x8EE348, blocked);   // the cutscene's hold back
        // The banner appears 5 s after it is summoned (screen +0x2C = tick of the request, 4000 ticks/s,
        // NotificationMessage 0x595E80 waits until 5 s have passed). Backdate it so ours shows at once.
        void* reg = nullptr; DWORD now = 0;
        if (Mem::Read(0x91D21C, reg) && reg && Mem::Read(0x925B14, now)) {
            uint8_t* pkg = ((uint8_t*(__thiscall*)(void*, const char*))0x585820)(reg, "EA_Trax.fng");
            uint8_t* scr = nullptr;
            if (pkg && Mem::Read((uintptr_t)pkg + 0x1C, scr) && scr) {
                DWORD t0 = now - 20001;
                if (!Mem::Write((uintptr_t)scr + 0x2C, t0)) return false;
                ChyronHoldStart();
                return true;
            }
        }
        return false;     // not shown (yet): the caller uses the corner line
    } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Notice: exception in the game's chyron"); return false; }
}
