// NFS MW Together (MWT) for Need for Speed: Most Wanted (2005) v1.3
// Step 1: plugin loads, shows overlay, networking between two game instances.
// Step 2: "ghost car" - the other player's car is shown and moved in your world.

#include <winsock2.h>
#include <windows.h>
#include <string>
#include <cstdio>
#include "log.h"
#include "perf.h"
#include <vector>
#include <share.h>
#include "config.h"
#include "net.h"
#include "overlay.h"
#include "game.h"
#include "input.h"
#include "mem.h"

#ifndef MWT_VERSION
#define MWT_VERSION "0.3.0-dev"
#endif

namespace {
    char     g_dir[MAX_PATH];
    bool     g_netOk = false;
    bool     g_gameOk = false;
    uint32_t g_lastSend = 0;
    uint32_t g_lastOverlay = 0;
    bool     g_keyWasDown = false;
    bool     g_ghostKeyWasDown = false;
    bool     g_ghostOn = true;
    std::string g_spawnMsg;
    bool        g_bgBoot = false;             // booting without focus
    HWND        g_prevForeground = nullptr;   // the window that had focus when the game started
    std::string g_toast;                      // (no longer shown: notices use the game's banner only)
    std::string g_pendingNotice; uint32_t g_pendingAt = 0;   // a notice waiting for the banner
    uint32_t    g_toastAt = 0;
    bool        g_coopHere = true;            // this profile may play co-op (career started, prologue done)
    const char* g_coopStatus = "";            // overlay line when co-op is off because of the prologue
    // A short notice for the player: in the game's own banner (the EA Trax chyron) when it can show one,
    // otherwise as a line in the corner (drawn by the overlay, also when the overlay is hidden).
    // The game's C++ runtime reports a fatal error (e.g. R6025 "pure virtual function call") in a box
    // with Abort / Retry / Ignore, none of which can save the game. The runtime finds MessageBoxA at run
    // time, so hook it at its entry (hot-patch prologue 8B FF 55 8B EC) and show one plain box instead.
    bool IsRuntimeError(const char* text, const char* caption) {
        auto has = [](const char* s, const char* w) { return s && strstr(s, w) != nullptr; };
        return has(caption, "Visual C++") || has(text, "Runtime Error") || has(text, "Debug Error") || has(text, "R6025") || has(text, "R6016");
    }
    int WINAPI hk_MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type) {
        if (IsRuntimeError(text, caption)) {
            LOG("CRASH: the game's runtime stopped it: %s", text ? text : "");
            Net::SendBye();
            std::string msg = "Need for Speed: Most Wanted crashed.\n\n";
            if (text) { const char* r = strstr(text, "R60"); msg += r ? r : text; msg += "\n\n"; }
            msg += "Please send scripts\\MWTogether.log from the game folder to the mod's authors.";
            MessageBoxExA(nullptr, msg.c_str(), "NFS MW Together", MB_OK | MB_ICONERROR | MB_TOPMOST, 0);
            TerminateProcess(GetCurrentProcess(), 3);
        }
        return MessageBoxExA(h, text, caption, type, 0);
    }
    void InstallCrashDialog() {
        uint8_t* f = (uint8_t*)GetProcAddress(GetModuleHandleA("user32.dll"), "MessageBoxA");
        const uint8_t pro[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
        if (!f || memcmp(f, pro, 5)) { LOG("Crash dialog: MessageBoxA not hooked (unexpected bytes)"); return; }
        uint8_t jmp[5] = { 0xE9 }; *(int32_t*)(jmp + 1) = (int32_t)((uintptr_t)&hk_MessageBoxA - ((uintptr_t)f + 5));
        DWORD old;
        if (VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &old)) { memcpy(f, jmp, 5); VirtualProtect(f, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), f, 5); LOG("Crash dialog: plain error box installed"); }
    }
    void Toast(const std::string& s) {
        bool inGame = g_cfg.gameNotices && g_gameOk && Game::ShowChyron("Most Wanted Together", s.c_str(), "");
        // only the game's own banner: where it can't show right now (loading, a menu), the notice waits
        // and appears as soon as it can (the newest one; dropped after 20 s)
        if (inGame) g_pendingNotice.clear(); else { g_pendingNotice = s; g_pendingAt = GetTickCount(); }
        LOG("Notice: %s%s", s.c_str(), inGame ? " (game banner)" : "");
    }

    const char* FlowName(int s) {
        switch (s) {
        case 6:  return "in game";
        case 3:  return "front end";
        case -1: return "unknown";
        default: return "loading/other";
        }
    }

    void OnFrame(IDirect3DDevice9*) {
        static bool firstFrame = true;
        if (firstFrame) {
            firstFrame = false;
            LOG("First frame rendered");
            g_gameOk = Game::Init();
            g_netOk = Net::Init();
        }
        // [Window] StartInBackground: once the game window exists, hand the focus back to the window
        // that had it before launch, and tell the game it's inactive (it mutes itself, like alt-tab).
        static int bgFrames = 0;
        if (g_cfg.startInBackground && bgFrames >= 0 && ++bgFrames > 3) {
            HWND gw = Overlay::GameWindow();
            if (gw) {
                bgFrames = -1;
                if (g_prevForeground && g_prevForeground != gw && IsWindow(g_prevForeground)) SetForegroundWindow(g_prevForeground);
                g_bgBoot = true;
                LOG("Window: started in the background (focus returned)");
            }
        }
        // While it boots in the background, keep the game's "application active" flag (0x982BEC) set
        // so its input polling and boot flow keep running (the profile screen is confirmed in boot.inl). Once in the world, tell it
        // that it's inactive (it mutes itself, like after alt-tab).
        if (g_bgBoot) {
            HWND gw = Overlay::GameWindow();
            if (GetForegroundWindow() == gw) g_bgBoot = false;            // the player clicked the game
            else if (Game::GameFlowState() == 6) {
                g_bgBoot = false;
                PostMessageA(gw, WM_ACTIVATE, WA_INACTIVE, 0);
                PostMessageA(gw, WM_ACTIVATEAPP, FALSE, 0);
                LOG("Window: in the world - muted until clicked");
            } else { int one = 1; Mem::Write(0x982BEC, one); }
        }

        Perf::FrameStart();
        { Perf::Scope perfMenus(Perf::Menus);
        static int lastFlow = -2;
        int flowNow = Game::GameFlowState();
        Input::Tick();
        FEScreens::Tick();
        if (g_gameOk) Game::BootTick();
        if (g_gameOk) Game::PursuitResultTick();
        if (g_gameOk) Pad::Tick(flowNow);
        if (g_gameOk) Game::ChyronTick();
        // the key-pressing AutoStart is only needed when the direct start is off (or for other targets)
        if (!(g_cfg.directFreeRoam && !_stricmp(g_cfg.autoStartTarget.c_str(), "freeroam"))) AutoStart::Tick(flowNow);
        if (g_gameOk) Game::OnFlow(flowNow);

        // Tell the peer which car (model + tuning) I'm driving; Net resends on change / every 2 s.
        static uint32_t lastCarRead = 0;
        if (g_netOk && g_gameOk && GetTickCount() - lastCarRead > 1000) {
            lastCarRead = GetTickCount();
            CarInfo ci;
            if (Game::ReadLocalCarInfo(ci)) Net::SetLocalCar(ci);
        }
        if (flowNow != lastFlow) {
            LOG("Game flow state %d -> %d (%s)", lastFlow, flowNow, FlowName(flowNow));
            lastFlow = flowNow;
        }

        }
        // Hotkeys: F8 overlay, F9 ghost on/off (only while game window focused)
        bool fg = Overlay::GameWindow() == GetForegroundWindow();   // hotkeys only for the focused game
        bool down = (fg && GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
        if (down && !g_keyWasDown) {
            Overlay::SetVisible(!Overlay::IsVisible());
            // remember it for the next launch
            WritePrivateProfileStringA("Debug", "Overlay", Overlay::IsVisible() ? "1" : "0", (std::string(g_dir) + "MWTogether.ini").c_str());
        }
        g_keyWasDown = down;
        bool gdown = (fg && g_cfg.testKeys && GetAsyncKeyState(VK_F9) & 0x8000) != 0;   // test key only: players hid each other by accident
        if (gdown && !g_ghostKeyWasDown) {
            g_ghostOn = !g_ghostOn;
            if (!g_ghostOn) Game::ReleaseGhost();
            LOG("Ghost %s", g_ghostOn ? "ON" : "OFF");
        }
        g_ghostKeyWasDown = gdown;

        // Debug: F10 spawn a clone of my car, F11 remove it
        static bool f10 = false, f11 = false;
        bool d10 = (fg && GetAsyncKeyState(VK_F10) & 0x8000) != 0, d11 = (fg && GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (g_cfg.testKeys && d10 && !f10) g_spawnMsg = Game::DebugSpawnClone();
        if (g_cfg.testKeys && d11 && !f11) g_spawnMsg = Game::DebugKillClone();
        static bool f7 = false;
        bool d7 = (fg && GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (g_cfg.testKeys && d7 && !f7) g_spawnMsg = Game::DebugStartEvent(g_cfg.testEvent.c_str());
        f7 = d7;
        static bool f5 = false;
        bool d5 = (fg && GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (g_cfg.testKeys && d5 && !f5 && Overlay::GameWindow() == GetForegroundWindow()) g_spawnMsg = Game::ToggleAutopilot();
        f5 = d5;
        static bool f12 = false;
        bool d12 = (fg && GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (g_cfg.testKeys && d12 && !f12) g_spawnMsg = Game::DebugSpawnCop();
        f12 = d12;
        f10 = d10; f11 = d11;

        // Test commands ([Debug] TestKeys=1): one command per line in scripts\MWTogether.cmd, the file is
        // deleted once read. Lets tests drive the game while it runs in the background.
        //   event <id> | events | restart | autopilot | cop | uncop | pursuit | clone | unclone | notice <text> | shot <file> | accept <package>
        //   | skipnis | story | story <movie> | milestones | milestone | speedtrap | flash
        static DWORD lastCmdCheck = 0;
        if (g_cfg.testKeys && g_gameOk && GetTickCount() - lastCmdCheck >= 500) {
            lastCmdCheck = GetTickCount();
            std::string cmdPath = std::string(g_dir) + "MWTogether.cmd";
            if (FILE* cf = _fsopen(cmdPath.c_str(), "r", _SH_DENYNO)) {
                char line[512];                 // long enough for a "shot <full path>"
                std::vector<std::string> cmds;
                while (fgets(line, sizeof(line), cf)) {
                    std::string s = line;
                    if (s.empty() || (s.back() != '\n' && !feof(cf))) {     // longer than the buffer: skip the whole line
                        int ch; while ((ch = fgetc(cf)) != EOF && ch != '\n') {}
                        LOG("Command: line too long - ignored");
                        continue;
                    }
                    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
                    if (!s.empty()) cmds.push_back(s);
                }
                fclose(cf);
                DeleteFileA(cmdPath.c_str());
                for (const std::string& c : cmds) {
                    std::string r;
                    if (c.rfind("event ", 0) == 0) r = Game::DebugStartEvent(c.substr(6).c_str());
                    else if (c == "event") r = Game::DebugStartEvent(g_cfg.testEvent.c_str());
                    else if (c == "events") r = Game::DebugListEvents();
                    else if (c == "restart") r = Game::DebugRestart();
                    else if (c == "autopilot") r = Game::ToggleAutopilot();
                    else if (c == "cop") r = Game::DebugSpawnCop();
                    else if (c == "uncop") r = Game::DebugKillCops();
                    else if (c == "ram") r = Game::DebugRam();
                    else if (c == "ram cop") r = Game::DebugRam(true);
                    else if (c == "pos") r = Game::DebugPos();
                    else if (c.rfind("tp ", 0) == 0) r = Game::DebugTeleport(c.substr(3).c_str());
                    else if (c == "skipnis") r = Game::DebugNisSkip();
                    else if (c == "story") r = Game::DebugStory("");
                    else if (c.rfind("story ", 0) == 0) r = Game::DebugStory(c.substr(6).c_str());
                    else if (c == "slowai") r = Game::DebugStopOpponents();
                    else if (c == "cash") r = Game::DebugCash();
                    else if (c == "milestones") r = Game::DebugMilestones("");
                    else if (c == "ground") r = Game::DebugGround();
                    else if (c == "milestone") r = Game::DebugMilestones("milestone");
                    else if (c == "speedtrap") r = Game::DebugMilestones("speedtrap");
                    else if (c == "pursuit") r = Game::DebugForcePursuit();
                    else if (c == "clone") r = Game::DebugSpawnClone();
                    else if (c == "unclone") r = Game::DebugKillClone();
                    else if (c.rfind("shot ", 0) == 0) { Overlay::Screenshot(c.substr(5)); r = "screenshot"; }
                    else if (c.rfind("accept ", 0) == 0) r = Game::FEAccept(c.substr(7).c_str());
                    else if (c.rfind("notice ", 0) == 0) { Toast(c.substr(7)); r = "notice shown"; }
                    else r = "unknown command";
                    LOG("Command '%s': %s", c.c_str(), r.c_str());
                    g_spawnMsg = r;
                }
            }
        }

        // --- networking ---
        CarState local{};
        bool haveLocal = false;
        { Perf::Scope perfNet(Perf::Net);
        if (g_netOk) {
            Net::Poll();
            Net::Tick();

            local.gameFlowState = Game::GameFlowState();
            g_coopHere = !g_gameOk || Game::CoopAllowed();
            haveLocal = g_gameOk && Game::ReadLocalCar(local);
            local.hasCar = (haveLocal && g_coopHere) ? 1 : 0;   // in the prologue the other side shows nothing
            local.nisHash = g_gameOk ? Game::SceneHash() : 0;      // also during scenes without a car (cutscene or story movie)
            local.nisVote = g_gameOk ? Game::NisVoteHash() : 0;
            local.eventHash = g_gameOk ? Game::CurrentEventHash() : 0;
            local.pad[0] = g_gameOk ? Game::RaceStage() : 0;
            local.pad[1] = g_coopHere ? 1 : 0;                    // co-op allowed here (past the prologue)
            local.pad[2] = g_gameOk ? Game::CareerBin() : 0;      // career rival number (story scenes the other game may show)
            if (g_gameOk) Game::FillSharedPursuit(local);          // [Coop] SharedPursuits: heat, pursuit state, busts

            static double nextSend = 0;
            double nowMs = Net::NowMs();
            if (nowMs >= nextSend) {
                nextSend = (nowMs - nextSend > 100 ? nowMs : nextSend) + 1000.0 / g_cfg.sendRateHz;
                Net::SendState(local);
            }
        }

        }
        // --- ghost ---
        std::string ghostStatus = "off";
        const NetStats& st = Net::Stats();
        // Co-op only while both players are past the prologue (pad[1] = 1 from a peer that allows it;
        // a peer we have no state from yet counts as allowed).
        const Net::Snapshot& peerSnap = Net::Latest();
        bool peerAllowed = !peerSnap.valid || peerSnap.s.pad[1] != 0;
        bool linked = st.connected && g_coopHere && peerAllowed;
        if (g_gameOk) Game::SetCoopLinked(linked);
        {
            static bool toldHere = false, toldPeer = false;
            if (st.connected && !g_coopHere && !toldHere) { toldHere = true; Toast("Finish the prologue first - co-op starts in free roam"); LOG("Co-op: off - this profile is still in the prologue / career not started"); }
            if (st.connected && g_coopHere && !peerAllowed && !toldPeer) {
                toldPeer = true;
                Toast((st.peerName.empty() ? std::string("The other player") : st.peerName) + " is still in the prologue - co-op starts when they reach free roam");
                LOG("Co-op: off - the other player is still in the prologue");
            }
            if (!st.connected) toldPeer = false;
            {   // one-way connection: we hear them, they don't hear us (their firewall / VPN), or nobody answers
                static bool toldOneWay = false, toldNoAnswer = false; static DWORD inGameSince = 0;
                DWORD now = GetTickCount();
                if (!st.connected) toldOneWay = false;
                else if (!toldOneWay && now - st.connectedTick > 10000 && now - st.peerHearsUsTick > 10000) {
                    toldOneWay = true;
                    std::string who = st.peerName.empty() ? std::string("The other player") : st.peerName;
                    Toast(who + " doesn't receive your game - allow UDP " + std::to_string(g_cfg.port) + " in the firewall on their PC");
                    LOG("Net: one-way - we receive the other player, they don't receive us (their firewall / VPN blocks UDP %d)", g_cfg.port);
                }
                if (g_gameOk && Game::GameFlowState() == 6) { if (!inGameSince) inGameSince = now; } else inGameSince = 0;
                if (st.connected) toldNoAnswer = false;
                else if (!g_cfg.isHost && !toldNoAnswer && inGameSince && now - inGameSince > 20000) {
                    toldNoAnswer = true;
                    Toast("No answer from the host - check the host's IP, and allow UDP " + std::to_string(g_cfg.port) + " in the firewall on this PC");
                    LOG("Net: no answer from the host %s - wrong IP, or a firewall blocks UDP %d", g_cfg.peerIp.c_str(), g_cfg.port);
                }
            }
            {   // careers at different Blacklist rivals: say so once (the rival screens and milestones differ then)
                static int toldMine = -1, toldTheirs = -1;
                const Net::Snapshot& ps = Net::Latest();
                int mine = g_gameOk ? Game::CareerBin() : 0, theirs = ps.valid ? ps.s.pad[2] : 0;
                if (st.connected && linked && mine >= 1 && mine <= 15 && theirs >= 1 && theirs <= 15 && mine != theirs &&
                    (mine != toldMine || theirs != toldTheirs)) {
                    toldMine = mine; toldTheirs = theirs;
                    std::string who = st.peerName.empty() ? std::string("The other player") : st.peerName;
                    Toast(who + " is at Blacklist #" + std::to_string(theirs) + ", you are at #" + std::to_string(mine) +
                          (mine > theirs ? " - beat your rival to catch up" : " - they need to beat their rival to catch up"));
                    LOG("Career: different rivals - ours #%d, theirs #%d", mine, theirs);
                }
            }
            // say when it changes: off -> on (both past the prologue) and on -> off
            static int lastLinked = -1;
            int nowLinked = st.connected ? (linked ? 1 : 0) : -1;
            if (nowLinked != lastLinked) {
                if (nowLinked == 1 && lastLinked == 0) {
                    Toast("Co-op on - " + (st.peerName.empty() ? std::string("the other player") : st.peerName) + " is in your world now");
                    LOG("Co-op: on - both players are past the prologue");
                } else if (nowLinked == 0) {
                    LOG("Co-op: off (%s)", !g_coopHere ? "this profile is in the prologue" : "the other player is in the prologue");
                }
                lastLinked = nowLinked;
            }
            g_coopStatus = !st.connected ? "" : !g_coopHere ? "off - finish the prologue first" : !peerAllowed ? "off - the other player is in the prologue" : "";
        }
        { Perf::Scope perfGhost(Perf::Ghost);
        if (g_netOk && g_gameOk && linked) {
            ghostStatus = Game::UpdateGhost(Net::Latest(), Net::Previous(), g_ghostOn && g_cfg.ghostEnabled);
            {   // the log says why the other car isn't there (the overlay may be hidden)
                static std::string lastGhost; static DWORD lastAt = 0;
                std::string kind = ghostStatus.substr(0, ghostStatus.find(" ("));
                if (ghostStatus.rfind("waiting", 0) == 0 && ghostStatus != lastGhost && GetTickCount() - lastAt > 3000) {
                    const Net::Snapshot& ps = Net::Latest();
                    LOG("Ghost: %s | peer state %s, hasCar %d, flow %d, co-op allowed there %d",
                        ghostStatus.c_str(), ps.valid ? "yes" : "none", ps.valid ? ps.s.hasCar : -1, ps.valid ? ps.s.gameFlowState : -1, ps.valid ? ps.s.pad[1] : -1);
                    lastAt = GetTickCount();
                }
                if (ghostStatus != lastGhost) lastGhost = ghostStatus;
                (void)kind;
            }
        } else if (g_gameOk) {
            Game::ReleaseGhost();
        }
        }
        { Perf::Scope p(Perf::Traffic); if (g_netOk && g_gameOk) Game::TrafficTick(linked, Net::Latest()); }
        { Perf::Scope p(Perf::Pursuit); if (g_netOk && g_gameOk) Game::PursuitTick(linked, Net::Latest()); }
        { Perf::Scope perfRules(Perf::Rules);
        if (g_netOk && g_gameOk) Game::EventTick(linked);
        if (g_netOk && g_gameOk) Game::CoopRulesTick(linked);
        if (g_netOk && g_gameOk) Game::WeatherTick(linked);
        if (g_netOk && g_gameOk) Game::FollowTick(linked);
        if (g_netOk && g_gameOk) Game::PauseRuleTick(linked);
        if (g_gameOk) Game::AutopilotTick();
        if (g_netOk && g_gameOk) Game::NisTick(linked);
        if (g_netOk && g_gameOk) Game::StoryTick(linked);
        if (g_netOk && g_gameOk) Game::SaveTick(linked);
        if (g_netOk && g_gameOk) Game::CoopWinTick(linked, Net::Latest());
        if (g_netOk && g_gameOk) Game::MilestoneTick(linked);
        Macro::Tick();
        }

        // --- join / leave notices ---
        {
            static bool wasConnected = false;
            static bool namePending = false;   // "Player joined" is up, waiting for the real name
            static std::string lastPeer;
            if (st.connected && !st.peerName.empty()) lastPeer = st.peerName;
            if (st.connected != wasConnected) {
                if (st.connected) Toast((st.peerName.empty() ? std::string("Player") : st.peerName) + " joined");
                else Toast((lastPeer.empty() ? std::string("The other player") : lastPeer) + (Net::PeerSaidBye() ? " left" : " lost connection"));
                namePending = st.connected && st.peerName.empty();
                wasConnected = st.connected;
            } else if (st.connected && namePending && !st.peerName.empty()) {
                namePending = false;
                Toast(st.peerName + " joined");   // the name arrives right after the connection
            }
        }
        // --- the other player is in an event while we roam (or back): say so ---
        // Debounced: the new state must hold for 3 s while we have been roaming for 3 s, so the
        // start and end of a shared event (both switch within moments) say nothing.
        {
            static int shown = -1, pending = -1;
            static uint32_t pendingAt = 0, meRacingAt = 0;
            const Net::Snapshot& pr = Net::Latest();
            uint32_t nowT = GetTickCount();
            bool meRacing = (haveLocal && local.inRace) || Game::CurrentEventHash() != 0;   // also while loading into an event
            if (meRacing || !haveLocal) meRacingAt = nowT;
            if (!st.connected) { shown = pending = -1; }
            else if (pr.valid && pr.s.hasCar && pr.s.gameFlowState == 6) {     // only clear samples count
                int v = (pr.s.inRace && !pr.s.finished) ? 1 : 0;   // finished = on the results screen, not racing
                if (shown < 0) shown = v;
                if (v != pending) { pending = v; pendingAt = nowT; }
                if (pending != shown && nowT - pendingAt > 3000 && nowT - meRacingAt > 3000) {
                    shown = pending;
                    std::string who = st.peerName.empty() ? std::string("The other player") : st.peerName;
                    Toast(shown ? who + " is in a race - you'll go together in the next event" : who + " is back in free roam");
                }
            }
        }
        if (g_gameOk && Game::TakeCoopWinNotice()) Toast((st.peerName.empty() ? std::string("Your partner") : st.peerName) + " won the race");
        if (g_gameOk) {
            std::string ms = Game::TakeMilestoneNotice();
            if (!ms.empty()) Toast((st.peerName.empty() ? std::string("Your partner") : st.peerName) + " " + ms);
        }
        {   // connection problems the player should know about (shown once per message)
            static std::string lastErr;
            std::string ne = (g_netOk && !st.connected) ? Net::LastError() : "";
            if (ne != lastErr) { if (!ne.empty()) Toast("Can't connect: " + ne); lastErr = ne; }
        }
        {   // waiting on the grid for the other player
            static std::string lastStart;
            std::string ss = g_gameOk ? Game::StartStatus() : "";
            if (ss != lastStart) { if (!ss.empty()) Toast(ss); lastStart = ss; }
        }
        {   // cutscene skip votes
            static std::string lastNis;
            std::string ns = g_gameOk ? Game::NisStatus() : "";
            if (ns != lastNis) { if (!ns.empty()) Toast("Cutscene: " + ns); lastNis = ns; }
        }
        {   // the other player's story scene (wait / watch along)
            static std::string lastStory;
            std::string ss = g_gameOk ? Game::StoryStatus() : "";
            if (ss != lastStory) { if (!ss.empty()) Toast(ss); lastStory = ss; }
        }
        if (!g_pendingNotice.empty()) {   // a notice that couldn't show yet: try the banner again
            static DWORD nextTry = 0;
            if (GetTickCount() - g_pendingAt > 20000) g_pendingNotice.clear();
            else if (GetTickCount() >= nextTry) {
                nextTry = GetTickCount() + 500;
                if (g_cfg.gameNotices && g_gameOk && Game::ShowChyron("Most Wanted Together", g_pendingNotice.c_str(), "")) g_pendingNotice.clear();
            }
        }
        bool toastOn = false;   // the black corner box is gone
        Overlay::SetForceShow(toastOn);

        // --- overlay text (rebuild ~5x per second, texture upload is not free) ---
        uint32_t now = GetTickCount();
        Perf::Scope perfOverlay(Perf::Overlay);
        if (toastOn && !Overlay::IsVisible() && now - g_lastOverlay > 200) {
            g_lastOverlay = now;
            Overlay::SetText("MW Together: " + g_toast + "\n");
        } else if (Overlay::IsVisible() && now - g_lastOverlay > 200) {
            g_lastOverlay = now;
            char buf[1024];
            std::string s;
            if (toastOn) s += ">> " + g_toast + "\n";
            sprintf_s(buf, "MW Together %s  [%s]  F8: hide  F9: ghost %s\n", MWT_VERSION,
                      g_cfg.isHost ? "HOST" : "CLIENT", g_ghostOn ? "ON" : "OFF");
            s += buf;

            if (!g_netOk) {
                sprintf_s(buf, "Network ERROR: %s\n", Net::LastError());
            } else if (st.connected) {
                sprintf_s(buf, "Connected: %s (%s)  ping %d ms  in/out %u/%u\n",
                          st.peerName.empty() ? "?" : st.peerName.c_str(), st.peerAddr.c_str(),
                          st.pingMs, st.packetsIn, st.packetsOut);
            } else if (g_cfg.isHost) {
                sprintf_s(buf, "Waiting for player on UDP port %d...\n", g_cfg.port);
            } else {
                sprintf_s(buf, "Connecting to %s:%d...\n", g_cfg.peerIp.c_str(), g_cfg.port);
            }
            s += buf;
            // why we are not connected, when the network told us (other mod version, our own address)
            if (g_netOk && !st.connected && Net::LastError()[0]) { s += "Problem: "; s += Net::LastError(); s += "\n"; }

            int flow = Game::GameFlowState();
            sprintf_s(buf, "Game: %s (%d)\n", FlowName(flow), flow);
            s += buf;
            if (haveLocal) {
                sprintf_s(buf, "You:  pos %.1f %.1f %.1f  %.0f km/h\n", local.pos[0], local.pos[1], local.pos[2], local.speedMps * 3.6f);
                s += buf;
            }
            const Net::Snapshot& r = Net::Latest();
            if (st.connected && r.valid) {
                if (r.s.hasCar)
                    sprintf_s(buf, "Peer: pos %.1f %.1f %.1f  %.0f km/h\n", r.s.pos[0], r.s.pos[1], r.s.pos[2], r.s.speedMps * 3.6f);
                else
                    sprintf_s(buf, "Peer: not driving (%s)\n", FlowName(r.s.gameFlowState));
                s += buf;
            }
            s += "Ghost: " + ghostStatus + "\n";
            if (st.connected && flow == 6) {
                char tb[96];
                if (g_cfg.isHost) sprintf_s(tb, "Traffic: sending %d cars\n", Game::TrafficCount());
                else sprintf_s(tb, "Traffic: %s, %d mirror cars\n", Game::TrafficSuppressed() ? "host's" : "own", Game::TrafficCount());
                s += tb;
            }
            s += std::string("AutoStart: ") + AutoStart::Status() + "\n";
            if (Game::NisStatus()[0]) s += std::string("Cutscene: ") + Game::NisStatus() + "\n";
            if (g_gameOk && Game::StoryStatus()[0]) s += std::string("Story: ") + Game::StoryStatus() + "\n";
            if (g_coopStatus[0]) s += std::string("Co-op: ") + g_coopStatus + "\n";
            if (Game::SaveStatus()[0]) s += std::string("Career: ") + Game::SaveStatus() + "\n";
            if (Game::EventStatus()[0]) s += std::string("Event: ") + Game::EventStatus() + "\n";
            if (flow == 3 && FEScreens::Current()[0]) s += std::string("Screen: ") + FEScreens::Current() + "\n";
            if (!g_spawnMsg.empty()) s += "Spawn test (F10/F11): " + g_spawnMsg + "\n";
            s += Game::DebugInfo();
            s += Perf::Summary();
            Overlay::SetText(s);
        }
    }
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);

        GetModuleFileNameA(mod, g_dir, MAX_PATH);
        char* slash = strrchr(g_dir, '\\');
        if (slash) slash[1] = 0;

        std::string logPath = std::string(g_dir) + "MWTogether.log";
        std::string iniPath = std::string(g_dir) + "MWTogether.ini";
        // renamed from CoopMod: carry the old settings over once
        if (GetFileAttributesA(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyFileA((std::string(g_dir) + "CoopMod.ini").c_str(), iniPath.c_str(), TRUE);
        // first start: the release ships MWTogether.default.ini, so an update never overwrites your settings
        if (GetFileAttributesA(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyFileA((std::string(g_dir) + "MWTogether.default.ini").c_str(), iniPath.c_str(), TRUE);
        Log::Init(logPath.c_str());
        LOG("NFS MW Together %s loading (dir %s)", MWT_VERSION, g_dir);

        g_cfg.Load(iniPath.c_str());
        Net::SetDataDir(g_dir);
        Game::SetDataDir(g_dir);

        // Front-end tweaks (code patches; applied before any menu exists, only if the bytes match v1.3).
        Perf::Enable(g_cfg.perf);
        g_prevForeground = GetForegroundWindow();
        Game::InstallBootPatches();
        InstallCrashDialog();
        Pad::InstallHooks();               // controller bindings guard (before the game creates its input devices)
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e) -> LONG {
            LOG("CRASH: exception %08X at %p", (unsigned)e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
            return EXCEPTION_CONTINUE_SEARCH;
        });
        if (g_cfg.runInBackground) {
            // message pump: always PeekMessage, never block in GetMessage while the window is inactive
            const uint8_t orig[2] = { 0x74, 0x0E }, patch[2] = { 0x90, 0x90 };
            LOG("Coop: keep running in background %s", Mem::PatchBytes(0x6C2245, orig, patch, 2) ? "enabled" : "NOT patched (bytes differ)");
        }
        if (g_cfg.uiIntroInput) {
            // ExecuteResponse 0x5BBD51: ignore the "disable input" sent by each screen's INTRO (message
            // 0x13FD3296); keep the locks after confirm/back so nothing can be activated twice.
            static const uint8_t orig[23] = { 0x85,0xFF,0x0F,0x84,0x97,0x00,0x00,0x00,0x83,0x7E,0x04,0x01,0x0F,0x94,0xC0,0x88,0x47,0x28,0xE9,0x88,0x00,0x00,0x00 };
            static const uint8_t patch[23] = { 0x85,0xFF,0x74,0xF7,0x81,0x7D,0x0C,0x96,0x32,0xFD,0x13,0x74,0xEE,0x83,0x7E,0x04,0x01,0x0F,0x94,0x47,0x28,0xEB,0xE4 };
            LOG("UI: input during menu intros %s", Mem::PatchBytes(0x5BBD51, orig, patch, 23) ? "enabled" : "NOT patched (bytes differ)");
        }
        if (g_cfg.uiMenuSpeed == 2 || g_cfg.uiMenuSpeed == 3) {
            // FE time step = real delta * 1200.0 (fmul [0x8970BC]) at 0x516A01 / 0x56440D: point both at an
            // existing 2400.0 (0x8ABE70) or 3600.0 (0x8A8AE8) constant.
            static const uint8_t orig[4] = { 0xBC,0x70,0x89,0x00 };
            uint32_t target = g_cfg.uiMenuSpeed == 2 ? 0x8ABE70 : 0x8A8AE8;
            bool a = Mem::PatchBytes(0x516A01, orig, &target, 4);
            bool b = Mem::PatchBytes(0x56440D, orig, &target, 4);
            LOG("UI: menu animations %dx %s", g_cfg.uiMenuSpeed, (a && b) ? "enabled" : "NOT fully patched");
        }
        g_ghostOn = g_cfg.ghostEnabled;
        Overlay::SetVisible(g_cfg.showOverlay);
        LOG("Config: mode=%s hostip=%s port=%d name=%s", g_cfg.isHost ? "host" : "client",
            g_cfg.peerIp.c_str(), g_cfg.port, g_cfg.playerName.c_str());

        // Sanity check: v1.3 speed.exe has its entry point at 0x7C4040.
        // (Patching the IAT is just a memory write, safe inside DllMain.)
        Overlay::SetText("MW Together " MWT_VERSION " loading...");
        Overlay::SetFrameCallback(&OnFrame);
        Overlay::SetCloseCallback([]() { if (g_netOk) Net::SendBye(); });
        if (!Input::InstallHooks())
            LOG("Failed to hook DirectInput - AutoStart disabled");
        if (!Overlay::InstallHooks())
            LOG("Failed to install D3D hooks - overlay and frame tick disabled");
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_netOk) Net::Shutdown();
    }
    return TRUE;
}
