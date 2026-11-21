// Config::Load: defaults, parsing, clamping. Works on temporary .ini files.
#include "net_helpers.h"
#include "../src/perf.h"

namespace {
    struct TempIni {
        std::string dir, path;
        explicit TempIni(const char* text) {
            dir = th::TempDir();
            static int n = 0;
            char name[64]; sprintf_s(name, "cfg_%d.ini", ++n);
            path = dir + name;
            FILE* f = nullptr;
            fopen_s(&f, path.c_str(), "wb");
            if (f) { fwrite(text, 1, strlen(text), f); fclose(f); }
        }
        ~TempIni() { DeleteFileA(path.c_str()); RemoveDirectoryA(dir.c_str()); }
    };
    Config LoadText(const char* text) {
        TempIni ini(text);
        Config c;
        c.Load(ini.path.c_str());
        return c;
    }
}

TEST(config_defaults_when_file_is_missing) {
    Config c;
    c.Load((th::TempDir() + "does_not_exist.ini").c_str());
    CHECK(c.isHost);
    CHECK_STR(c.peerIp, "127.0.0.1");
    CHECK_STR(c.clientIp, "");
    CHECK_EQ(c.port, 47555);
    CHECK_EQ(c.sendRateHz, 60);
    CHECK_STR(c.playerName, "Player");
    CHECK(c.ghostEnabled);
    CHECK_EQ(c.interpDelayMs, 100);
    CHECK(c.ghostCollisions);
    CHECK_EQ(c.safeDistance, 10);
    CHECK(c.trafficSync);
    CHECK_EQ(c.trafficMirrorClass, 4);
    CHECK(c.trafficSharedInRaces);
    CHECK(c.trafficSmoothHandover);
    CHECK(c.trafficNoSpawnNearPlayers);
    CHECK(c.noSpeedbreaker); CHECK(c.noPause); CHECK(c.runInBackground); CHECK(c.streamAroundPeer);
    CHECK(!c.skipIntro); CHECK(c.directFreeRoam); CHECK(c.skipLeaveSafeHouseDialog);
    CHECK(c.autoStart);
    CHECK_STR(c.autoStartTarget, "freeroam");
    CHECK(c.uiIntroInput);
    CHECK_EQ(c.uiMenuSpeed, 2);
    CHECK(!c.skipPursuitResults);
    CHECK(c.gameNotices);
    CHECK_EQ(c.noticeSeconds, 10);
    CHECK(c.gamepadHotplug); CHECK(c.keepPadBindings);
    CHECK_EQ(c.syncStoryScenes, 1);
    CHECK(c.sharedPursuits); CHECK(c.noPursuitCams);
    CHECK(c.bossHelper); CHECK(!c.bossHelperSolo); CHECK(c.coopPrologue);
    CHECK(c.hideCursor);
    CHECK(!c.startInBackground);
    // debug options are all off unless asked for
    CHECK(!c.showOverlay); CHECK(!c.testKeys); CHECK(!c.autoPilot); CHECK(!c.perf);
    CHECK_EQ(c.toggleKey, 0x77);                     // F8
    CHECK_STR(c.testEvent, "15.1.1");
    RemoveDirectoryA(th::TempDir().c_str());
}

TEST(config_parses_values) {
    Config c = LoadText(
        "[Network]\r\nMode=client\r\nHostIP=192.168.1.20\r\nClientIP=192.168.1.21\r\nPort=50000\r\nSendRate=30\r\n"
        "[Player]\r\nName=Kostya\r\n"
        "[Ghost]\r\nEnabled=0\r\nInterpDelayMs=150\r\nCollisions=0\r\nSafeDistance=25\r\n"
        "[Traffic]\r\nSync=0\r\nMirrorClass=6\r\nSharedInRaces=0\r\nSmoothHandover=0\r\nNoSpawnNearPlayers=0\r\n"
        "[Coop]\r\nNoSpeedbreaker=0\r\nNoPause=0\r\nRunInBackground=0\r\nStreamAroundPeer=0\r\nSyncStoryScenes=2\r\nSharedPursuits=0\r\nNoPursuitCams=0\r\nBossHelper=0\r\n"
        "[Start]\r\nSkipIntro=0\r\nDirectFreeRoam=0\r\nSkipLeaveSafeHouseDialog=0\r\n"
        "[AutoStart]\r\nEnabled=0\r\nTarget=race\r\nDelayMs=10\r\nIntervalMs=20\r\nSequence=ENTER,ENTER\r\n"
        "[UI]\r\nIntroInput=0\r\nMenuSpeed=3\r\nSkipPursuitResults=1\r\nNoticeSeconds=15\r\n"
        "[Input]\r\nGamepadHotplug=0\r\nKeepPadBindings=0\r\n"
        "[Window]\r\nHideCursor=0\r\nStartInBackground=1\r\n"
        "[Debug]\r\nOverlay=1\r\nToggleKey=0x7A\r\nTestKeys=1\r\nAutoPilot=1\r\nPerf=1\r\nTestEvent=1.2.3\r\nBossHelperSolo=1\r\n");
    CHECK(!c.isHost);
    CHECK_STR(c.peerIp, "192.168.1.20");
    CHECK_STR(c.clientIp, "192.168.1.21");
    CHECK_EQ(c.port, 50000);
    CHECK_EQ(c.sendRateHz, 30);
    CHECK_STR(c.playerName, "Kostya");
    CHECK(!c.ghostEnabled);
    CHECK_EQ(c.interpDelayMs, 150);
    CHECK(!c.ghostCollisions);
    CHECK_EQ(c.safeDistance, 25);
    CHECK(!c.trafficSync);
    CHECK_EQ(c.trafficMirrorClass, 6);
    CHECK(!c.trafficSharedInRaces);
    CHECK(!c.trafficSmoothHandover);
    CHECK(!c.trafficNoSpawnNearPlayers);
    CHECK(!c.noSpeedbreaker); CHECK(!c.noPause); CHECK(!c.runInBackground); CHECK(!c.streamAroundPeer);
    CHECK(!c.skipIntro); CHECK(!c.directFreeRoam); CHECK(!c.skipLeaveSafeHouseDialog);
    CHECK(!c.autoStart);
    CHECK_STR(c.autoStartTarget, "race");
    CHECK_EQ(c.autoStartDelayMs, 10);
    CHECK_EQ(c.autoStartIntervalMs, 20);
    CHECK_STR(c.autoStartSequence, "ENTER,ENTER");
    CHECK(!c.uiIntroInput);
    CHECK_EQ(c.uiMenuSpeed, 3);
    CHECK(c.skipPursuitResults);
    CHECK_EQ(c.noticeSeconds, 15);
    CHECK(!c.gamepadHotplug); CHECK(!c.keepPadBindings);
    CHECK_EQ(c.syncStoryScenes, 2);
    CHECK(!c.sharedPursuits); CHECK(!c.noPursuitCams);
    CHECK(!c.bossHelper); CHECK(c.bossHelperSolo);
    CHECK(!c.hideCursor);
    CHECK(c.startInBackground);
    CHECK(c.showOverlay); CHECK(c.testKeys); CHECK(c.autoPilot); CHECK(c.perf);
    CHECK_EQ(c.toggleKey, 0x7A);                     // hex is accepted
    CHECK_STR(c.testEvent, "1.2.3");
}

TEST(config_mode_is_case_insensitive_and_defaults_to_host) {
    CHECK(!LoadText("[Network]\nMode=CLIENT\n").isHost);
    CHECK(!LoadText("[Network]\nMode=Client\n").isHost);
    CHECK(LoadText("[Network]\nMode=host\n").isHost);
    CHECK(LoadText("[Network]\nMode=server\n").isHost);      // anything else: host
    CHECK(LoadText("[Network]\nMode=\n").isHost);
}

TEST(config_send_rate_is_clamped) {
    CHECK_EQ(LoadText("[Network]\nSendRate=0\n").sendRateHz, 1);     // main.cpp divides by it
    CHECK_EQ(LoadText("[Network]\nSendRate=-20\n").sendRateHz, 1);
    CHECK_EQ(LoadText("[Network]\nSendRate=abc\n").sendRateHz, 1);   // not a number reads as 0
    CHECK_EQ(LoadText("[Network]\nSendRate=1\n").sendRateHz, 1);
    CHECK_EQ(LoadText("[Network]\nSendRate=120\n").sendRateHz, 120);
    CHECK_EQ(LoadText("[Network]\nSendRate=121\n").sendRateHz, 120);
    CHECK_EQ(LoadText("[Network]\nSendRate=100000\n").sendRateHz, 120);
    CHECK_EQ(LoadText("[Network]\nSendRate=\n").sendRateHz, 60);     // empty: the default
}

TEST(config_port_must_be_a_valid_udp_port) {
    CHECK_EQ(LoadText("[Network]\nPort=1\n").port, 1);
    CHECK_EQ(LoadText("[Network]\nPort=65535\n").port, 65535);
    CHECK_EQ(LoadText("[Network]\nPort=0\n").port, 47555);
    CHECK_EQ(LoadText("[Network]\nPort=-1\n").port, 47555);
    CHECK_EQ(LoadText("[Network]\nPort=65536\n").port, 47555);       // would wrap to port 0
    CHECK_EQ(LoadText("[Network]\nPort=113091\n").port, 47555);      // would wrap to 47555 by accident
    CHECK_EQ(LoadText("[Network]\nPort=junk\n").port, 47555);
}

TEST(config_sync_story_scenes_range) {
    CHECK_EQ(LoadText("[Coop]\nSyncStoryScenes=0\n").syncStoryScenes, 0);
    CHECK_EQ(LoadText("[Coop]\nSyncStoryScenes=2\n").syncStoryScenes, 2);
    CHECK_EQ(LoadText("[Coop]\nSyncStoryScenes=7\n").syncStoryScenes, 1);     // out of range: the default
    CHECK_EQ(LoadText("[Coop]\nSyncStoryScenes=-1\n").syncStoryScenes, 1);
}

TEST(config_notice_seconds_range) {
    CHECK_EQ(LoadText("[UI]\nNoticeSeconds=2\n").noticeSeconds, 2);
    CHECK_EQ(LoadText("[UI]\nNoticeSeconds=60\n").noticeSeconds, 60);
    CHECK_EQ(LoadText("[UI]\nNoticeSeconds=1\n").noticeSeconds, 2);
    CHECK_EQ(LoadText("[UI]\nNoticeSeconds=0\n").noticeSeconds, 2);
    CHECK_EQ(LoadText("[UI]\nNoticeSeconds=600\n").noticeSeconds, 60);
    CHECK_EQ(LoadText("[UI]\nNoticeSeconds=\n").noticeSeconds, 10);       // empty: the default
}

TEST(config_ghost_values_are_clamped) {
    CHECK_EQ(LoadText("[Ghost]\nInterpDelayMs=-50\n").interpDelayMs, 0);
    CHECK_EQ(LoadText("[Ghost]\nInterpDelayMs=0\n").interpDelayMs, 0);
    CHECK_EQ(LoadText("[Ghost]\nInterpDelayMs=1000\n").interpDelayMs, 1000);
    CHECK_EQ(LoadText("[Ghost]\nInterpDelayMs=99999\n").interpDelayMs, 1000);
    CHECK_EQ(LoadText("[Ghost]\nSafeDistance=-5\n").safeDistance, 0);
    CHECK_EQ(LoadText("[Ghost]\nSafeDistance=40\n").safeDistance, 40);
}

TEST(config_long_and_odd_strings) {
    std::string longName(300, 'n');
    Config c = LoadText(("[Player]\nName=" + longName + "\n[Network]\nHostIP=  my-host.example  \n").c_str());
    CHECK_EQ(c.playerName.size(), 300);              // kept whole here; cut where it is used (hello: 23, HUD: 15)
    CHECK_STR(c.peerIp, "my-host.example");          // surrounding blanks are dropped by the ini reader
    char hud[NetUtil::HUD_NAME_SIZE];
    NetUtil::CopyHudName(hud, c.playerName.c_str());
    CHECK_EQ(strlen(hud), 15);

    c = LoadText("; a comment\n[Player]\n; Name=Commented\nName=\n");
    CHECK_STR(c.playerName, "");                     // present but empty stays empty
    c = LoadText("garbage without sections\n\x01\x02\x03\n[[[\n");
    CHECK_STR(c.playerName, "Player");
    CHECK_EQ(c.port, 47555);
}

TEST(config_shipped_ini_matches_built_in_defaults) {
    // MWTogether.ini in the repository is what players get: it must load to the same values as no
    // file at all (apart from the AutoStart timings it tunes on purpose).
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir.erase(dir.find_last_of('\\') + 1);
    std::string ini = dir + "..\\MWTogether.ini";
    if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("    (skipped: %s not found - run tests.exe from the repository's build folder)\n", ini.c_str());
        return;
    }
    char full[MAX_PATH];
    GetFullPathNameA(ini.c_str(), MAX_PATH, full, nullptr);
    Config shipped, def;
    shipped.Load(full);
    def.Load((th::TempDir() + "does_not_exist.ini").c_str());
    RemoveDirectoryA(th::TempDir().c_str());
    CHECK_EQ(shipped.isHost, def.isHost);
    CHECK_STR(shipped.peerIp, def.peerIp);
    CHECK_STR(shipped.clientIp, def.clientIp);
    CHECK_EQ(shipped.port, def.port);
    CHECK_EQ(shipped.sendRateHz, def.sendRateHz);
    CHECK_STR(shipped.playerName, def.playerName);
    CHECK_EQ(shipped.ghostEnabled, def.ghostEnabled);
    CHECK_EQ(shipped.interpDelayMs, def.interpDelayMs);
    CHECK_EQ(shipped.ghostCollisions, def.ghostCollisions);
    CHECK_EQ(shipped.safeDistance, def.safeDistance);
    CHECK_EQ(shipped.trafficSync, def.trafficSync);
    CHECK_EQ(shipped.trafficMirrorClass, def.trafficMirrorClass);
    CHECK_EQ(shipped.trafficSharedInRaces, def.trafficSharedInRaces);
    CHECK_EQ(shipped.trafficSmoothHandover, def.trafficSmoothHandover);
    CHECK_EQ(shipped.trafficNoSpawnNearPlayers, def.trafficNoSpawnNearPlayers);
    CHECK_EQ(shipped.noSpeedbreaker, def.noSpeedbreaker);
    CHECK_EQ(shipped.noPause, def.noPause);
    CHECK_EQ(shipped.runInBackground, def.runInBackground);
    CHECK_EQ(shipped.streamAroundPeer, def.streamAroundPeer);
    CHECK_EQ(shipped.skipIntro, def.skipIntro);
    CHECK_EQ(shipped.bossHelper, def.bossHelper);
    CHECK_EQ(shipped.coopPrologue, def.coopPrologue);
    CHECK_EQ(shipped.directFreeRoam, def.directFreeRoam);
    CHECK_EQ(shipped.skipLeaveSafeHouseDialog, def.skipLeaveSafeHouseDialog);
    CHECK_EQ(shipped.autoStart, def.autoStart);
    CHECK_STR(shipped.autoStartTarget, def.autoStartTarget);
    CHECK_STR(shipped.autoStartSequence, def.autoStartSequence);
    CHECK_EQ(shipped.uiIntroInput, def.uiIntroInput);
    CHECK_EQ(shipped.uiMenuSpeed, def.uiMenuSpeed);
    CHECK_EQ(shipped.skipPursuitResults, def.skipPursuitResults);
    CHECK_EQ(shipped.syncStoryScenes, def.syncStoryScenes);
    CHECK_EQ(shipped.hideCursor, def.hideCursor);
    CHECK_EQ(shipped.startInBackground, def.startInBackground);
    CHECK_EQ(shipped.showOverlay, def.showOverlay);
    CHECK_EQ(shipped.testKeys, def.testKeys);
    CHECK_EQ(shipped.autoPilot, def.autoPilot);
    CHECK_EQ(shipped.perf, def.perf);
    CHECK_EQ(shipped.toggleKey, def.toggleKey);
}

// ---- perf.h (header-only, no game needed) -------------------------------------------------------
TEST(perf_off_by_default_and_frame_buffer_is_bounded) {
    Perf::State& s = Perf::S();
    s = Perf::State{};
    Perf::FrameStart();
    { Perf::Scope p(Perf::Net); }
    CHECK_EQ(s.frames, 0);                           // disabled: costs nothing, records nothing
    CHECK(Perf::Summary().empty());

    Perf::Enable(true);
    for (int i = 0; i < 6000; ++i) Perf::FrameStart();   // more frames in one window than the buffer holds
    CHECK(s.frames <= 4096);
    CHECK(s.frames > 0);
    { Perf::Scope p(Perf::Traffic); Sleep(2); }
    CHECK(s.secMs[Perf::Traffic] >= 1.0);
    CHECK(s.secMax[Perf::Traffic] >= 1.0);
    CHECK(s.modMs >= 1.0);
    CHECK_NEAR(s.secMs[Perf::Net], 0, 1e-9);

    // pretend the 5 s window is over: one summary line, counters start again
    s.windowStart.QuadPart -= 6 * s.qpf.QuadPart;
    Perf::FrameStart();
    CHECK(!Perf::Summary().empty());
    CHECK(Perf::Summary().find("fps") != std::string::npos);
    CHECK_EQ(s.frames, 0);
    CHECK_NEAR(s.modMs, 0, 1e-9);
    for (int i = 0; i < Perf::Count; ++i) CHECK(Perf::Name(i) != nullptr && Perf::Name(i)[0]);
    s = Perf::State{};
}
