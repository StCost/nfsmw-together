#include "config.h"
#include <windows.h>
#include <cstdlib>

Config g_cfg;

static std::string ReadStr(const char* ini, const char* sec, const char* key, const char* def) {
    char buf[1024];
    GetPrivateProfileStringA(sec, key, def, buf, sizeof(buf), ini);
    return buf;
}

static int ReadInt(const char* ini, const char* sec, const char* key, int def) {
    std::string s = ReadStr(ini, sec, key, "");
    if (s.empty()) return def;
    return (int)strtol(s.c_str(), nullptr, 0); // accepts 0x77 or 119
}

void Config::Load(const char* ini) {
    std::string mode = ReadStr(ini, "Network", "Mode", "host");
    isHost       = _stricmp(mode.c_str(), "client") != 0;
    peerIp       = ReadStr(ini, "Network", "HostIP", "127.0.0.1");
    clientIp     = ReadStr(ini, "Network", "ClientIP", "");
    port         = ReadInt(ini, "Network", "Port", 47555);
    sendRateHz   = ReadInt(ini, "Network", "SendRate", 60);
    playerName   = ReadStr(ini, "Player", "Name", "Player");
    {   // "RRGGBB" or "#RRGGBB"; empty = the game's own colour
        std::string c = ReadStr(ini, "Player", "Color", "");
        if (!c.empty() && c[0] == '#') c.erase(0, 1);
        char* end = nullptr;
        unsigned long v = c.size() == 6 ? strtoul(c.c_str(), &end, 16) : 0;
        playerColor = (end && *end == 0) ? (0xFF000000u | (uint32_t)(v & 0xFFFFFF)) : 0;
    }
    autoStart    = ReadInt(ini, "AutoStart", "Enabled", 1) != 0;
    autoStartDelayMs    = ReadInt(ini, "AutoStart", "DelayMs", 2500);
    autoStartIntervalMs = ReadInt(ini, "AutoStart", "IntervalMs", 1500);
    autoStartSequence   = ReadStr(ini, "AutoStart", "Sequence", "@WS_MW_LS_Splash.fng,ENTER,@MC_Main.fng,ENTER,@MainMenu.fng,ENTER,@MainMenu_Sub.fng,ENTER,WAIT,WAIT,WAIT,WAIT,ENTER,@Dialog.fng,LEFT,ENTER");
    hideCursor   = ReadInt(ini, "Window", "HideCursor", 1) != 0;
    showOverlay  = ReadInt(ini, "Debug", "Overlay", 0) != 0;
    testKeys     = ReadInt(ini, "Debug", "TestKeys", 0) != 0;
    noSpeedbreaker = ReadInt(ini, "Coop", "NoSpeedbreaker", 1) != 0;
    noPause = ReadInt(ini, "Coop", "NoPause", 1) != 0;
    runInBackground = ReadInt(ini, "Coop", "RunInBackground", 1) != 0;
    autoPilot = ReadInt(ini, "Debug", "AutoPilot", 0) != 0;
    autoPilotNos = ReadInt(ini, "Debug", "AutoPilotNos", 1) != 0;
    syncControls = ReadInt(ini, "Ghost", "SyncControls", 1) != 0;
    syncDamage = ReadInt(ini, "Ghost", "SyncDamage", 1) != 0;
    perf = ReadInt(ini, "Debug", "Perf", 0) != 0;
    bootTrace = ReadInt(ini, "Debug", "BootTrace", 0) != 0;
    saveSync = ReadInt(ini, "Career", "SaveSync", 1) != 0;
    sharedWins = ReadInt(ini, "Career", "SharedWins", 1) != 0;
    startTogether = ReadInt(ini, "Coop", "StartTogether", 1) != 0;
    catchUpLeader = ReadInt(ini, "Coop", "CatchUpLeader", 1) != 0;
    noSlowMotion = ReadInt(ini, "Coop", "NoSlowMotion", 1) != 0;
    syncWeather = ReadInt(ini, "Coop", "SyncWeather", 1) != 0;
    sharedPursuits = ReadInt(ini, "Coop", "SharedPursuits", 1) != 0;
    noPursuitCams = ReadInt(ini, "Coop", "NoPursuitCams", 1) != 0;
    syncStoryScenes = ReadInt(ini, "Coop", "SyncStoryScenes", 1);
    if (syncStoryScenes < 0 || syncStoryScenes > 2) syncStoryScenes = 1;
    readyDelayMs = ReadInt(ini, "Debug", "ReadyDelayMs", 0);
    startInBackground = ReadInt(ini, "Window", "StartInBackground", 0) != 0;
    skipPursuitResults = ReadInt(ini, "UI", "SkipPursuitResults", 0) != 0;
    gameNotices = ReadInt(ini, "UI", "GameNotices", 1) != 0;
    noticeSeconds = ReadInt(ini, "UI", "NoticeSeconds", 10);
    if (noticeSeconds < 2) noticeSeconds = 2;
    if (noticeSeconds > 60) noticeSeconds = 60;
    gamepadHotplug = ReadInt(ini, "Input", "GamepadHotplug", 1) != 0;
    keepPadBindings = ReadInt(ini, "Input", "KeepPadBindings", 1) != 0;
    skipIntro = ReadInt(ini, "Start", "SkipIntro", 0) != 0;
    directFreeRoam = ReadInt(ini, "Start", "DirectFreeRoam", 1) != 0;
    skipLoadComplete = ReadInt(ini, "Start", "SkipLoadComplete", 1) != 0;
    skipLeaveSafeHouseDialog = ReadInt(ini, "Start", "SkipLeaveSafeHouseDialog", 1) != 0;
    streamAroundPeer = ReadInt(ini, "Coop", "StreamAroundPeer", 1) != 0;
    uiIntroInput = ReadInt(ini, "UI", "IntroInput", 1) != 0;
    uiMenuSpeed  = ReadInt(ini, "UI", "MenuSpeed", 2);
    autoStartTarget = ReadStr(ini, "AutoStart", "Target", "freeroam");
    testEvent    = ReadStr(ini, "Debug", "TestEvent", "15.1.1");
    trafficSync  = ReadInt(ini, "Traffic", "Sync", 1) != 0;
    trafficMirrorClass = ReadInt(ini, "Traffic", "MirrorClass", 4);
    trafficSharedInRaces = ReadInt(ini, "Traffic", "SharedInRaces", 1) != 0;
    trafficSmoothHandover = ReadInt(ini, "Traffic", "SmoothHandover", 1) != 0;
    toggleKey    = ReadInt(ini, "Debug", "ToggleKey", 0x77);
    ghostEnabled = ReadInt(ini, "Ghost", "Enabled", 1) != 0;
    interpDelayMs= ReadInt(ini, "Ghost", "InterpDelayMs", 100);
    ghostCollisions = ReadInt(ini, "Ghost", "Collisions", 1) != 0;
    safeDistance = ReadInt(ini, "Ghost", "SafeDistance", 10);
    if (sendRateHz < 1) sendRateHz = 1;
    if (sendRateHz > 120) sendRateHz = 120;
    if (port < 1 || port > 65535) port = 47555;        // would wrap when cast to a 16-bit port
    if (interpDelayMs < 0) interpDelayMs = 0;          // negative = showing the future (all extrapolation)
    if (interpDelayMs > 1000) interpDelayMs = 1000;
    if (safeDistance < 0) safeDistance = 0;
}
