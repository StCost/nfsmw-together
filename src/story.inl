// Included from game.cpp (after nis.inl).
// ---------------------------------------------------------------------------
// Story scenes outside events ([Coop] SyncStoryScenes)
//  The career shows its story in three ways:
//   - in-game movies (FMV over the world): PlayInGameMovie 0x57E470 (cdecl, name) copies the name to
//     0x8F3F40, sets the "movie playing" byte 0x91CF68 and pushes InGameAnyMovie.fng (screen vtable
//     0x8A0FC8; a two-movie variant 0x8A0FD4 reads its name from 0x8F3F80). The byte is cleared when
//     the movie ends or is skipped (0x56B5C0 / 0x56B6A0, screen destructors). Used by EPlayRaceMovie
//     (0x62F820), e.g. the movie an in-world NIS ends with (INIS+0x354).
//   - front-end movies in the Safe House (Blacklist rival movies "blacklist_%02d" from the career
//     sequence 0x5302C0, storyfmv_rap30 / saf25): name in 0x8F3750, FEAnyMovie.fng (vtable 0x89E3C4).
//   - in-world NIS scenes (NIS manager, nis.inl). Outside events only the scenes in kStoryNis count
//     (arrests, pursuit breakers and replays are not story).
//  Movie names are bare ("blacklist_05", "storyfmv_pin11"); we send bStringHash (0x460BF0: h*33+c,
//  start -1) of the lower-case name in CarState::nisHash, the career rival number (+0xB0) in pad[2].
//  The movie screens' notification handler (vtable slot 1) gets PAD_ACCEPT / PAD_START to skip:
//  we hook that slot for the skip vote (skip when both voted, as for race intro cutscenes).
//  The other player's game waits meanwhile: the front-end pause request counter 0x91CAE4 (as the
//  start-together hold in event.inl), and with SyncStoryScenes=2 plays the same movie when its own
//  career has reached it.
//  Starting one ourselves (watch along, test command "story <movie>"):
//   - 0x57E470 sets the byte first, then pushes the movie screen in both cases: SwitchPackage
//     (0x525940 -> 0x5BBB40) when the screen named in 0x8F4318 is the top input screen (0x516B90 walks
//     cFEng's package list for the first one with +0x20 set and compares its name), PushPackage
//     (0x5257F0 -> 0x5BBB60) otherwise. In free roam no package has +0x20 set (the HUD takes no menu
//     input; a menu or dialog does - e.g. Pause_Main), and 0x8F4318 holds the last loading screen's
//     name, so the push path is the normal one. We don't start over an open menu / dialog, and clear the
//     byte if no screen came up (else the world stays stopped: the sim gate 0x6EF150 tests the byte at
//     0x6EF159, before the co-op no-pause patch at 0x6EF1B1).
//   - The game option SKIPMOVIES (0x926144; the Widescreen Fix's SkipIntro = 1 sets it) makes every movie
//     send "movie done" at once - story movies too. Then we don't start any.
//   - The career only starts these over the world from an NIS (EPlayRaceMovie 0x62F820, created by INIS
//     at 0x6F56DB / 0x6EA8FF), i.e. with the world held and the streamer settled. The screen ctor
//     (0x58F180) waits for the track streamer (0x9AA9D0) to go idle, and the movie library allocates
//     its buffers through 0x517470 from the streamer's pool (0x723A00, pool 7). With StreamAroundPeer
//     our streamer also keeps the other player's area loaded. While a story movie starts / plays we
//     stream only our own area (stream.inl g_streamQuiet) and start it 0.4 s after that.
//   - The movie screen closes only through its slot 1 (0x57E400): message 0xC3960EB9 ("movie done",
//     sent to every package by the movie player at 0x542BC9 when it can't open the movie, 0x542CFC when
//     playback ends, 0x5597F8 when the file is missing) or PAD_ACCEPT / PAD_START (skip, always allowed
//     in the career), or by being popped (destructor 0x57E390 clears the byte). We log which one it was
//     with the movie player's state (*0x91CB10: +0x11C/+0x120 state, 5 = playing; +0x140 stream,
//     +0x144 first frame).
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t MoviePlayingFlag = 0x91CF68, InGameMovieName = 0x8F3F40, InGameMovieName2 = 0x8F3F80,
                        FEMovieName = 0x8F3750, PlayInGameMovieFn = 0x57E470, PauseRequests = 0x91CAE4,
                        InGameMovieVt = 0x8A0FC8, InGameMovie2Vt = 0x8A0FD4, FEMovieVt = 0x89E3C4,
                        MoviePlayerPtr = 0x91CB10,
                        SkipMoviesOption = 0x926144;   // SKIPMOVIES (read at boot, 0x78A9E2): every movie ends at once (0x542A56, 0x5596A2)
    constexpr uint32_t PadAccept = 0x406415E3, PadStart = 0xB5AF2461, MovieDone = 0xC3960EB9;

    const char* const kStoryMovies[] = {
        "blacklist_01", "blacklist_02", "blacklist_03", "blacklist_04", "blacklist_05", "blacklist_06",
        "blacklist_07", "blacklist_08", "blacklist_09", "blacklist_10", "blacklist_11", "blacklist_12",
        "blacklist_13", "blacklist_14", "blacklist_15",
        "storyfmv_bla134", "storyfmv_bus12", "storyfmv_cro06_coh06a", "storyfmv_dda01", "storyfmv_epi138",
        "storyfmv_her136", "storyfmv_pin11", "storyfmv_pol17_mot21", "storyfmv_rap30", "storyfmv_raz08",
        "storyfmv_roc02", "storyfmv_saf25", "storyfmv_rac01",
    };
    // in-world story NIS (NIS\Scene_<name>_BundleB.bun); the name case the game hashes is not known,
    // so each is matched as written, upper and lower case
    const char* const kStoryNis[] = {
        "FMV17to21", "FMV20T1", "FMVBLIntro", "FMVNoCars", "FMVPIN11X06", "FMVPSA", "FMVroc02",
        "EndingNis01", "EndingNis02", "EndingNis03", "EndingNis04",
    };
    constexpr int kMovies = sizeof(kStoryMovies) / sizeof(kStoryMovies[0]);
    constexpr int kNis = sizeof(kStoryNis) / sizeof(kStoryNis[0]);
    uint32_t g_movieHashes[kMovies], g_nisHashes[kNis * 3];
    bool     g_hashesReady = false;

    uint32_t BHash(const char* s) { uint32_t h = 0xFFFFFFFFu; while (*s) h = h * 33 + (uint8_t)*s++; return h; }
    void InitStoryHashes() {
        if (g_hashesReady) return;
        for (int i = 0; i < kMovies; ++i) g_movieHashes[i] = BHash(kStoryMovies[i]);
        for (int i = 0; i < kNis; ++i) {
            char up[32], lo[32];
            strcpy_s(up, kStoryNis[i]); strcpy_s(lo, kStoryNis[i]);
            _strupr_s(up); _strlwr_s(lo);
            g_nisHashes[i * 3] = BHash(kStoryNis[i]); g_nisHashes[i * 3 + 1] = BHash(up); g_nisHashes[i * 3 + 2] = BHash(lo);
        }
        g_hashesReady = true;
    }
    const char* KnownMovie(uint32_t h) {
        InitStoryHashes();
        for (int i = 0; h && i < kMovies; ++i) if (g_movieHashes[i] == h) return kStoryMovies[i];
        return nullptr;
    }
    bool StoryNis(uint32_t h) {
        InitStoryHashes();
        for (int i = 0; h && i < kNis * 3; ++i) if (g_nisHashes[i] == h) return true;
        return false;
    }

    // ---- the movie this game shows right now ----
    char        g_movieName[64] = "";
    uint32_t    g_movieHash = 0;          // story movie playing here (0 = none / not a story movie)
    bool        g_movieInGame = false;    // over the world (pauses it) or in the Safe House front end
    const char* g_moviePkg = nullptr;     // its front-end package (to send it the skip button)
    uintptr_t   g_lastMovieVt = 0;        // vtable of the movie screen that last got a message
    DWORD       g_lastMoviePoll = 0;
    DWORD       g_playingSince = 0;       // the "movie playing" byte is set since (0 = isn't)
    uint32_t    g_lastCloseMsg = 0; DWORD g_lastCloseMsgAt = 0;   // last close / skip message a movie screen got

    // "MOVIES\Blacklist_05.vp6" -> "blacklist_05"
    void NormalizeMovie(const char* src, char* out, size_t n) {
        const char* s = src;
        for (const char* p = src; *p; ++p) if (*p == '\\' || *p == '/') s = p + 1;
        size_t i = 0;
        for (; s[i] && i + 1 < n; ++i) { char c = s[i]; out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
        out[i] = 0;
        if (char* dot = strstr(out, ".vp6")) *dot = 0;
    }
    bool IsStoryMovieName(const char* s) { return !strncmp(s, "blacklist_", 10) || !strncmp(s, "storyfmv_", 9); }

    void PollMovieRaw() {
        uint8_t playing = 0;
        Mem::Read(MoviePlayingFlag, playing);
        static const char* const pkgs[4] = { "InGameAnyMovie.fng", "WS_InGameAnyMovie.fng", "FEAnyMovie.fng", "WS_FEAnyMovie.fng" };
        g_moviePkg = nullptr;
        for (const char* p : pkgs) if (FEScreens::IsPushed(p)) { g_moviePkg = p; break; }
        char raw[64] = "";
        uintptr_t src = 0;
        if (!playing && g_lastMovieVt != FEMovieVt) g_lastMovieVt = 0;   // the next in-game movie may be the other class
        if (playing) src = g_lastMovieVt == InGameMovie2Vt ? InGameMovieName2 : InGameMovieName;
        else if (g_moviePkg && strstr(g_moviePkg, "FEAnyMovie")) src = FEMovieName;
        if (src && Mem::SafeRead((const void*)src, raw, sizeof(raw) - 1)) raw[sizeof(raw) - 1] = 0; else raw[0] = 0;
        NormalizeMovie(raw, g_movieName, sizeof(g_movieName));
        if (!playing) g_playingSince = 0; else if (!g_playingSince) g_playingSince = GetTickCount();
        bool shown = g_moviePkg || (playing && GetTickCount() - g_playingSince < 1000);   // a set byte with no screen is not a movie
        g_movieInGame = playing != 0 && shown;
        g_movieHash = (src && shown && g_movieName[0] && IsStoryMovieName(g_movieName)) ? BHash(g_movieName) : 0;
    }
    void PollMovie(bool force) {
        DWORD now = GetTickCount();
        if (!force && now - g_lastMoviePoll < 200) return;
        g_lastMoviePoll = now;
        __try { PollMovieRaw(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_movieHash = 0; }
    }

    uint8_t CareerBinRaw() {
        uint8_t* fedb = nullptr; uint8_t* up = nullptr;
        if (!Mem::Read(0x91CF90, fedb) || !fedb || !fedb[3] || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up) return 0;
        return up[0xB0];
    }

    // my story scene (0 = none): a story movie, or a story NIS outside events
    uint32_t MyStoryScene() {
        if (g_movieHash) return g_movieHash;
        uint32_t n = Game::NisSceneHash();
        return (n && !Game::CurrentEventHash() && StoryNis(n)) ? n : 0;
    }
    uint32_t PeerStoryScene() {
        const Net::Snapshot& p = Net::Latest();
        if (!p.valid || !p.s.nisHash || p.s.eventHash) return 0;
        return (KnownMovie(p.s.nisHash) || StoryNis(p.s.nisHash)) ? p.s.nisHash : 0;
    }

    // ---- skip vote on movies ----
    using MovieNotifyFn = void(__thiscall*)(void*, uint32_t, void*, uint32_t, uint32_t);
    MovieNotifyFn g_movieNotify[3] = {};
    bool     g_wsfSkipsMovies = false;     // the Widescreen Fix's SkipIntro = 1 (see InstallStoryHooks)
    uint32_t g_movieForceSkip = 0;        // let this skip through (the other player has skipped / voted)

    // what the movie player (*0x91CB10) was doing when a movie screen got a close / skip message
    void LogMovieClose(uintptr_t vt, uint32_t msg) {
        DWORD now = GetTickCount();
        if (msg == g_lastCloseMsg && now - g_lastCloseMsgAt < 1000) { g_lastCloseMsgAt = now; return; }   // broadcast / repeated
        g_lastCloseMsg = msg; g_lastCloseMsgAt = now;
        uint8_t* mp = nullptr; int st = -1, st2 = -1; void* strm = nullptr; void* frame = nullptr;
        if (Mem::Read(MoviePlayerPtr, mp) && mp) {
            Mem::Read((uintptr_t)mp + 0x11C, st); Mem::Read((uintptr_t)mp + 0x120, st2);
            Mem::Read((uintptr_t)mp + 0x140, strm); Mem::Read((uintptr_t)mp + 0x144, frame);
        }
        const char* what = msg == MovieDone ? "movie done (0xC3960EB9)" : msg == PadAccept ? "PAD_ACCEPT" : "PAD_START";
        LOG("Story: %s movie screen got %s %u ms after the start (player %p state %d/%d stream %p frame %p)",
            vt == InGameMovieVt ? "in-game" : vt == InGameMovie2Vt ? "in-game (2)" : "front-end", what,
            g_playingSince ? (unsigned)(now - g_playingSince) : 0u, mp, st, st2, strm, frame);
    }

    bool MovieMessageAllowed(void* self, uint32_t msg) {
        __try {
            uintptr_t vt = 0; Mem::Read((uintptr_t)self, vt);
            g_lastMovieVt = vt;
            if (msg == MovieDone || msg == PadAccept || msg == PadStart) LogMovieClose(vt, msg);
            if (msg != PadAccept && msg != PadStart) return true;
            if (g_cfg.syncStoryScenes <= 0 || !g_nisConnected) return true;
            PollMovie(true);
            uint32_t h = g_movieHash;
            const Net::Snapshot& p = Net::Latest();
            if (!h || !p.valid || p.s.nisHash != h) return true;               // watching alone: like the game
            if (p.s.nisVote == h || g_movieForceSkip == h) {
                LOG("Story: %s - skipping %s", g_movieForceSkip == h ? "the other player is past it" : "both voted", g_movieName);
                g_movieForceSkip = 0; Skipped(h);
                return true;
            }
            if (g_myVote != h) { g_myVote = h; LOG("Story: voted to skip %s - waiting for the other player", g_movieName); }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    }
    template <int I>
    void __fastcall hk_MovieNotify(void* self, void*, uint32_t msg, void* obj, uint32_t a, uint32_t b) {
        if (MovieMessageAllowed(self, msg)) g_movieNotify[I](self, msg, obj, a, b);
    }

    // in-game movie screens' scalar deleting destructors (vtable slot 0): a screen that goes away
    // without a close message was popped by someone else
    using MovieDtorFn = void*(__thiscall*)(void*, unsigned);
    MovieDtorFn g_movieDtor[2] = {};
    void NoteMovieScreenGone() {
        __try {
            DWORD now = GetTickCount();
            bool byMsg = g_lastCloseMsg && now - g_lastCloseMsgAt < 1500;
            LOG("Story: in-game movie screen closed %u ms after the start (%s)",
                g_playingSince ? (unsigned)(now - g_playingSince) : 0u,
                !byMsg ? "popped without a close message" : g_lastCloseMsg == MovieDone ? "after movie done" : "skipped");
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    template <int I>
    void* __fastcall hk_MovieDtor(void* self, void*, unsigned flags) {
        NoteMovieScreenGone();
        return g_movieDtor[I](self, flags);
    }

    // ---- the other player watches: wait, or watch along ----
    bool     g_storyHold = false;
    DWORD    g_storyHoldSince = 0, g_peerStorySince = 0, g_startAt = 0, g_startedAt = 0, g_doneUntil = 0;
    uint32_t g_peerStory = 0, g_startedFor = 0, g_startPending = 0;
    uint32_t g_cmdPending = 0; DWORD g_cmdAt = 0;   // test command "story <movie>": start pending
    bool     g_screenSeen = false;                  // our last start brought its screen up
    DWORD    g_lastSkipSend = 0, g_peerLeftSince = 0;
    uint32_t g_gaveUpFor = 0;                 // scene we stopped waiting for (safety limit): not held again
    uint32_t g_lastWatched = 0; DWORD g_lastWatchedAt = 0;   // a scene we just finished here: not replayed or waited for
    char     g_storyStatus[128] = "";

    const char* PeerName() { const std::string& n = Net::Stats().peerName; return n.empty() ? "The other player" : n.c_str(); }

    void StoryHoldOn() {
        if (g_storyHold) return;
        int req = 0;
        if (!Mem::Read(PauseRequests, req)) return;
        ++req; Mem::Write(PauseRequests, req);
        g_storyHold = true; g_storyHoldSince = GetTickCount();
        LOG("Story: the other player is watching a story scene (%08X) - holding the world", g_peerStory);
    }
    void StoryHoldOff(const char* why) {
        if (!g_storyHold) return;
        g_storyHold = false;
        int req = 0;
        if (Mem::Read(PauseRequests, req) && req > 0) { --req; Mem::Write(PauseRequests, req); }
        LOG("Story: hold released (%s, %.0f s)", why, (GetTickCount() - g_storyHoldSince) / 1000.f);
    }

    bool MovieFileExists(const char* name) {
        char dir[MAX_PATH]; GetModuleFileNameA(nullptr, dir, MAX_PATH);
        char* slash = strrchr(dir, '\\'); if (slash) slash[1] = 0;
        char pat[MAX_PATH]; sprintf_s(pat, "%sMOVIES\\%s_*.vp6", dir, name);
        WIN32_FIND_DATAA fd; HANDLE f = FindFirstFileA(pat, &fd);
        if (f == INVALID_HANDLE_VALUE) return false;
        FindClose(f); return true;
    }
    // has my career reached this scene? (rival numbers count down: 15 first, 1 last)
    bool CareerHasMovie(const char* name, uint8_t peerBin) {
        uint8_t mine = CareerBinRaw();
        if (mine < 1 || mine > 15) return false;
        if (!strncmp(name, "blacklist_", 10)) { int nn = atoi(name + 10); return nn >= 1 && nn <= 15 && mine <= nn; }
        return peerBin >= 1 && peerBin <= 15 && mine <= peerBin;
    }
    bool PlayInGameMovie(const char* name) {
        __try { ((void(__cdecl*)(const char*))PlayInGameMovieFn)(name); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // The first package in cFEng's list that takes menu input (+0x20 set; walked like 0x516B90): a pause
    // menu, dialog, map ... nullptr in plain free roam.
    const char* InputScreenName(void* fe) {
        uint8_t* root = nullptr; uint8_t* e = nullptr;
        if (!Mem::Read((uintptr_t)fe, root) || !root || !Mem::Read((uintptr_t)root + 0xD8 + 0xC, e)) return nullptr;
        for (int i = 0; e && i < 64; ++i) {
            uint32_t active = 0; const char* name = nullptr;
            Mem::Read((uintptr_t)e + 0x20, active);
            if (active) { Mem::Read((uintptr_t)e + 0xC, name); return (name && Mem::IsReadable(name, 2)) ? name : "?"; }
            if (!Mem::Read((uintptr_t)e + 8, e)) break;
        }
        return nullptr;
    }
    // why the game would not show an in-game movie now (nullptr = it would)
    const char* MovieStartBlockerRaw() {
        if (Game::GameFlowState() != 6) return "not in the world";
        if (Game::NisSceneHash()) return "a cutscene is playing";
        uint8_t playing = 0; Mem::Read(MoviePlayingFlag, playing);
        if (playing) return "a movie is already playing";
        uint32_t skipMovies = 0; Mem::Read(SkipMoviesOption, skipMovies);
        if (skipMovies) return "movies are switched off in this game (SKIPMOVIES)";
        if (g_wsfSkipsMovies) return "the Widescreen Fix's SkipIntro = 1 switches off the game's movies";
        void* fe = nullptr;
        if (!Mem::Read(0x91CADC, fe) || !fe) return "no front end";
        if (const char* menu = InputScreenName(fe)) {
            static char why[96];
            sprintf_s(why, "a menu / dialog is open (%.48s)", menu);
            return why;
        }
        return nullptr;
    }
    const char* MovieStartBlocker() {
        __try { return MovieStartBlockerRaw(); } __except (EXCEPTION_EXECUTE_HANDLER) { return "exception"; }
    }

    // start a story movie over the world here, as the game would (see the notes at the top)
    bool FireOwnMovie(const char* name, const char* why) {
        if (const char* no = MovieStartBlocker()) { LOG("Story: not starting %s here - %s", name, no); return false; }
        if (!PlayInGameMovie(name)) { LOG("Story: exception starting %s", name); return false; }
        g_startedAt = GetTickCount(); g_playingSince = g_startedAt; g_screenSeen = false;
        LOG("Story: playing %s here (%s)", name, why);
        return true;
    }

    void StoryTickImpl(bool connected) {
        DWORD now = GetTickCount();
        PollMovie(false);
        {   // a forced skip belongs to one movie: drop it when another one (or none) plays
            static uint32_t lastMovie = 0;
            if (g_movieHash != lastMovie) { if (g_movieForceSkip != g_movieHash) g_movieForceSkip = 0; lastMovie = g_movieHash; }
        }
        const Net::Snapshot& p = Net::Latest();
        uint32_t mine = MyStoryScene();
        uint32_t peer = (connected && g_cfg.syncStoryScenes > 0) ? PeerStoryScene() : 0;
        if (peer != g_peerStory) {
            if (g_peerStory && !peer && g_storyHold) { sprintf_s(g_storyStatus, "%s finished the story scene", PeerName()); g_doneUntil = now + 3000; }
            g_peerStory = peer; g_peerStorySince = now;
            if (!peer) { g_startedFor = 0; g_startPending = 0; }
            if (!peer) g_gaveUpFor = 0;   // the safety limit holds for the whole run of scenes, not per scene
        }
        {   // remember the scene we just finished, so a partner still in it doesn't start it here again
            static uint32_t prevMine = 0;
            if (prevMine && mine != prevMine) { g_lastWatched = prevMine; g_lastWatchedAt = now; }
            prevMine = mine;
        }
        bool justWatched = peer && peer == g_lastWatched && now - g_lastWatchedAt < 60000;
        int flow = Game::GameFlowState();
        bool roaming = flow == 6 && !Game::CurrentEventHash() && !Game::NisSceneHash() && !g_movieInGame;

        // watch along (SyncStoryScenes=2): the same movie, when this career has reached it
        const char* movie = peer ? KnownMovie(peer) : nullptr;
        if (movie && !justWatched && g_cfg.syncStoryScenes >= 2 && roaming && !Game::LocalPursuitHeat() && g_startedFor != peer && !g_startPending &&
            now - g_peerStorySince > 300 && CareerHasMovie(movie, p.s.pad[2]) && MovieFileExists(movie)) {
            g_startPending = peer; g_startAt = now + 400;     // our streamer drops the other player's area first
            g_streamQuiet = true;
            StoryHoldOff("starting the same movie here");   // a pushed screen must find no pause request of ours
        }
        if (g_startPending && (int)(now - g_startAt) >= 0) {
            uint32_t h = g_startPending; g_startPending = 0; g_startedFor = h;
            const char* name = KnownMovie(h);
            if (name && roaming) FireOwnMovie(name, "the other player is watching it");
        }
        if (g_cmdPending && (int)(now - g_cmdAt) >= 0) {
            uint32_t h = g_cmdPending; g_cmdPending = 0;
            if (const char* name = KnownMovie(h)) FireOwnMovie(name, "test command");
        }
        if (g_startedAt) {
            if (!g_screenSeen && g_moviePkg && g_movieInGame) {
                g_screenSeen = true;
                LOG("Story: movie screen %s is up (%u ms)", g_moviePkg, (unsigned)(now - g_startedAt));
            }
            // a start that pushed no screen leaves the "movie playing" byte set, and the byte alone stops
            // the world (0x6EF159): clear it. (g_movieInGame is already false then - read the byte itself.)
            if (now - g_startedAt > 2500) {
                g_startedAt = 0;
                PollMovie(true);
                uint8_t b = 0;
                bool rd = Mem::Read(MoviePlayingFlag, b);
                if (rd && b && !g_moviePkg) { uint8_t z = 0; Mem::Write(MoviePlayingFlag, z); LOG("Story: the movie screen did not come up here - cleared the movie byte"); }
                else if (rd && !b) LOG("Story: our movie is over within 2.5 s (screen %s) - see the close reason above", g_screenSeen ? "was up" : "never seen");
            }
        }
        // stream only our own area while a story movie starts / plays over the world here
        g_streamQuiet = g_startPending || g_cmdPending || g_startedAt || (g_movieHash && g_movieInGame);

        // otherwise wait: hold the world while they watch (free roam only)
        // (the 0.7 s settle only before a hold starts: a scene running straight into the next keeps it)
        bool want = peer && !mine && roaming && !g_startPending && !justWatched && !g_gaveUpFor &&
                    (g_storyHold || now - g_peerStorySince > 700);
        if (want && !g_storyHold) StoryHoldOn();
        if (g_storyHold) {
            if (!want) StoryHoldOff(peer ? "left free roam" : "scene over");
            else if (now - g_storyHoldSince > 8 * 60 * 1000) { StoryHoldOff("safety limit"); g_startedFor = peer; g_gaveUpFor = peer; }
            else { int req = 0; if (Mem::Read(PauseRequests, req) && req <= 0) { req = 1; Mem::Write(PauseRequests, req); } }  // a closed menu took ours
        }

        // my vote on a movie: skip once the other player votes too, or has left it
        if (g_myVote && g_myVote == g_movieHash && connected) {
            bool gone = !p.valid || p.s.nisHash != g_myVote;
            if (!gone) g_peerLeftSince = 0; else if (!g_peerLeftSince) g_peerLeftSince = now;
            bool go = (p.valid && p.s.nisVote == g_myVote) || (gone && now - g_peerLeftSince > 500);
            if (go && g_moviePkg && now - g_lastSkipSend > 500) {
                g_lastSkipSend = now;
                g_movieForceSkip = g_myVote;
                void* fe = nullptr;
                if (Mem::Read(0x91CADC, fe) && fe)
                    ((void(__thiscall*)(void*, uint32_t, const char*, uint32_t))0x516C50)(fe, PadAccept, g_moviePkg, 0xFF);
            }
        } else g_peerLeftSince = 0;

        // status line / notice
        if (g_storyHold) sprintf_s(g_storyStatus, "%s is watching a story scene - your game waits", PeerName());
        else if (peer && mine == peer && g_startedFor == peer) sprintf_s(g_storyStatus, "watching %s's story scene here too", PeerName());
        else if (peer && !mine && !g_startPending) sprintf_s(g_storyStatus, "%s is watching a story scene", PeerName());
        else if (g_doneUntil && (int)(g_doneUntil - now) > 0) {}                 // "... finished" for a moment
        else if (!g_startPending) { g_doneUntil = 0; g_storyStatus[0] = 0; }
    }
}

uint32_t Game::SceneHash() {
    if (!g_ok) return 0;
    if (g_cfg.syncStoryScenes > 0) { PollMovie(false); if (g_movieHash) return g_movieHash; }
    return NisSceneHash();
}

// Test 'careerbin <n>' (TestKeys=1, TEST PROFILES ONLY): set this career's current Blacklist rival
// (user profile +0xB0, 1..15) in memory, e.g. 1 to try Razor's events (1.x) together with `event`. The
// game saves it with its next autosave, so use it on a copy of a profile, never on a real career.
static bool SetCareerBinRaw(int n, int& before) {
    __try {
        uint8_t* fedb = nullptr; uint8_t* up = nullptr;
        if (!Mem::Read(0x91CF90, fedb) || !fedb || !fedb[3] || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up) return false;
        before = up[0xB0]; up[0xB0] = (uint8_t)n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string Game::DebugCareerBin(const char* arg) {
    if (!g_ok) return "not ready";
    int n = arg ? atoi(arg) : 0, before = 0;
    if (n < 1 || n > 15) return "usage: careerbin <1..15> (test profiles only)";
    if (!SetCareerBinRaw(n, before)) return "no profile loaded";
    LOG("Career: TEST - current Blacklist rival set from #%d to #%d (saved with the next autosave: test profiles only)", before, n);
    char r[96]; sprintf_s(r, "rival #%d -> #%d (test profiles only)", before, n);
    return r;
}

uint8_t Game::CareerBin() {
    if (!g_ok) return 0;
    __try { return CareerBinRaw(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void Game::StoryTick(bool connected) {
    if (!g_ok) return;
    __try { StoryTickImpl(connected); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        __try { StoryHoldOff("error"); } __except (EXCEPTION_EXECUTE_HANDLER) { g_storyHold = false; }   // never leave our pause request behind
        g_storyStatus[0] = 0;
        g_streamQuiet = false;
    }
    if (!connected || g_cfg.syncStoryScenes <= 0) { __try { StoryHoldOff("disconnected / off"); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
}

// the world must really pause while we hold, or while we watch a story movie over it
bool Game::StoryPauseWanted() { return g_storyHold || (g_cfg.syncStoryScenes > 0 && g_movieHash && g_movieInGame); }
const char* Game::StoryStatus() { return g_storyStatus; }

bool Game::InstallStoryHooks() {
    // The Widescreen Fix (scripts\NFSMostWanted.WidescreenFix.ini, [MISC] SkipIntro = 1) skips every movie,
    // story movies too (seen on the laptop: each ends 0 ms after the start). MW Together skips the intro
    // by itself, so only say so - the player can set the fix's SkipIntro = 0.
    {
        std::string wsf = std::string(g_dataDir) + "NFSMostWanted.WidescreenFix.ini";
        if (GetFileAttributesA(wsf.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetPrivateProfileIntA("MISC", "SkipIntro", 0, wsf.c_str()) != 0) {
            g_wsfSkipsMovies = true;
            LOG("Story: the Widescreen Fix has SkipIntro = 1 - the game plays no movies, story movies included (set it to 0 in NFSMostWanted.WidescreenFix.ini to keep them; MW Together skips the intro itself)");
        }
    }
    struct Slot { uintptr_t slot, expect; void* hook; };
    const Slot slots[3] = {
        { InGameMovieVt + 4, 0x57E400, (void*)&hk_MovieNotify<0> },
        { InGameMovie2Vt + 4, 0x57E4F0, (void*)&hk_MovieNotify<1> },
        { FEMovieVt + 4, 0x559880, (void*)&hk_MovieNotify<2> },
    };
    int ok = 0;
    for (int i = 0; i < 3; ++i) {
        uintptr_t cur = 0;
        if (!Mem::Read(slots[i].slot, cur) || cur != slots[i].expect) { LOG("Story: movie screen slot %08X holds %08X - not hooked", (unsigned)slots[i].slot, (unsigned)cur); continue; }
        g_movieNotify[i] = (MovieNotifyFn)cur;
        Mem::PatchPointer((void*)slots[i].slot, slots[i].hook);
        ++ok;
    }
    LOG("Story: movie skip voting hooked on %d/3 movie screens (SyncStoryScenes=%d)", ok, g_cfg.syncStoryScenes);
    // diagnostics: the in-game movie screens' destructors (a screen popped without a close message)
    const Slot dtors[2] = {
        { InGameMovieVt, 0x58F2A0, (void*)&hk_MovieDtor<0> },
        { InGameMovie2Vt, 0x58F5F0, (void*)&hk_MovieDtor<1> },
    };
    for (int i = 0; i < 2; ++i) {
        uintptr_t cur = 0;
        if (!Mem::Read(dtors[i].slot, cur) || cur != dtors[i].expect) { LOG("Story: movie screen dtor %08X holds %08X - not watched", (unsigned)dtors[i].slot, (unsigned)cur); continue; }
        g_movieDtor[i] = (MovieDtorFn)cur;
        Mem::PatchPointer((void*)dtors[i].slot, dtors[i].hook);
    }
    return ok == 3;
}

// Test command: "story" = what we see; "story <movie>" = play that movie over the world here (as the
// career would), e.g. story storyfmv_pin11 / story blacklist_15. It starts 0.4 s later (after our
// streamer has dropped the other player's area); the log tells how it went.
static const char* DebugStorySafe(const char* arg) {
    static char out[320];
    __try {
        if (arg && arg[0]) {
            char name[64]; NormalizeMovie(arg, name, sizeof(name));
            uint32_t h = BHash(name);
            if (!KnownMovie(h)) return "not a story movie name (blacklist_NN / storyfmv_...)";
            if (!MovieFileExists(name)) return "no such file in MOVIES";
            if (const char* no = MovieStartBlocker()) { sprintf_s(out, "not now: %s", no); return out; }
            if (g_cmdPending || g_startPending) return "a start is already pending";
            g_cmdPending = h; g_cmdAt = GetTickCount() + 400; g_streamQuiet = true;
            sprintf_s(out, "starting %s in 0.4 s (see the log)", name);
            return out;
        }
        PollMovie(true);
        uint8_t byte = 0; Mem::Read(MoviePlayingFlag, byte);
        const char* no = MovieStartBlocker();
        sprintf_s(out, "movie '%s' story %08X in-game %d byte %u pkg %s | scene %08X | peer story %08X | hold %d | rival %u | start: %s",
                  g_movieName, g_movieHash, g_movieInGame ? 1 : 0, (unsigned)byte, g_moviePkg ? g_moviePkg : "-",
                  Game::SceneHash(), PeerStoryScene(), g_storyHold ? 1 : 0, (unsigned)CareerBinRaw(), no ? no : "ok");
        return out;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "exception"; }
}
std::string Game::DebugStory(const char* arg) { return g_ok ? DebugStorySafe(arg) : "game not recognised"; }
