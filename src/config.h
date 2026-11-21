#pragma once
#include <string>

struct Config {
    bool        isHost = true;        // [Network] Mode = host | client
    std::string peerIp = "127.0.0.1"; // [Network] HostIP (client only)
    std::string clientIp;             // [Network] ClientIP (host only, optional): send to the client too,
                                      // so firewalls on both sides see the traffic as a reply
    int         port = 47555;         // [Network] Port (both sides bind it)
    int         sendRateHz = 60;      // [Network] SendRate
    std::string playerName = "Player";// [Player] Name
    uint32_t    playerColor = 0;         // [Player] Color - RRGGBB the other player sees our map/minimap blip in; 0 = not set (game colour)
    bool        autoStart = true;      // [AutoStart] Enabled - title screen -> career automatically
    int         autoStartDelayMs = 2500;   // [AutoStart] DelayMs - wait in the menu before the first press
    int         autoStartIntervalMs = 1500;// [AutoStart] IntervalMs - between presses
    std::string autoStartSequence = "@WS_MW_LS_Splash.fng,ENTER,@MC_Main.fng,ENTER,@MainMenu.fng,ENTER,@MainMenu_Sub.fng,ENTER,WAIT,WAIT,WAIT,WAIT,ENTER,@Dialog.fng,LEFT,ENTER"; // [AutoStart] Sequence
    bool        hideCursor = true;    // [Window] HideCursor - hide the Windows arrow over the game
    bool        showOverlay = true;   // [Debug] Overlay
    bool        testKeys = false;
    bool        noSpeedbreaker = true;   // [Coop] NoSpeedbreaker
    bool        noPause = true;          // [Coop] NoPause - the world keeps running while a pause menu / map is open (connected only)
    bool        streamAroundPeer = true; // [Coop] StreamAroundPeer - load the world around the other player (full physics there)
    bool        skipIntro = false;       // [Start] SkipIntro - no logos / intro movie (off by default)
    bool        directFreeRoam = true;
    bool        skipLoadComplete = true; // [Start] SkipLoadComplete - confirm the "load complete" screen by itself   // [Start] DirectFreeRoam - load career free roam right after the profile
    bool        skipLeaveSafeHouseDialog = true; // [Start] SkipLeaveSafeHouseDialog
    bool        gameNotices = true;       // [UI] GameNotices - notices in the game's own banner (EA Trax chyron)
    int         noticeSeconds = 10;       // [UI] NoticeSeconds - how long a notice stays on screen (2..60)
    bool        gamepadHotplug = true;    // [Input] GamepadHotplug - pick up pads connected while the game runs
    bool        keepPadBindings = true;   // [Input] KeepPadBindings - starting without the pad keeps its bindings
    bool        skipPursuitResults = false; // [UI] SkipPursuitResults - close the pursuit result screen in free roam
    bool        startInBackground = false; // [Window] StartInBackground - give focus back on start, game muted until clicked
    bool        restoreOnlyMissing = true; // [Career] RestoreOnlyMissing - the host restores our save only if it is gone
    bool        syncWeather = true;
    bool        followToPursuits = true; // [Coop] FollowToPursuits - come along when the other player starts a Blacklist pursuit      // [Coop] SyncWeather - the client shows the host's rain
    bool        sharedPursuits = true;   // [Coop] SharedPursuits - shared heat, busted together, cooldown / escape together,
                                         // the other player's cops on the radar, ramming their cop starts a pursuit
    bool        noPursuitCams = true;    // [Coop] NoPursuitCams - no pursuit-start camera (look at the cop) while connected
    bool        noSlowMotion = true;     // [Coop] NoSlowMotion - no slow-motion moments while connected (speed traps, pursuit / crash cams)
    bool        catchUpLeader = true;    // [Coop] CatchUpLeader - AI catch-up follows whichever player is ahead
    bool        bossHelper = true;       // [Coop] BossHelper - co-op Blacklist rival races get one more AI racer (normal AI)
    bool        bossHelperSolo = false;  // [Debug] BossHelperSolo - test: the extra rival-race racer also when not connected
    bool        startTogether = true;    // [Coop] StartTogether - wait on the grid until the other player has loaded
    bool        separateGridSlots = true; // [Coop] SeparateGridSlots - client: move off the grid slot both games give their own player
    int         syncStoryScenes = 1;     // [Coop] SyncStoryScenes - 0 off, 1 the other game waits while one watches a story scene
                                         // (+ skip only when both voted), 2 also plays the same story movie there
    int         readyDelayMs = 0;        // [Debug] ReadyDelayMs - test: report "ready" this much later
    bool        sharedWins = true;       // [Career] SharedWins - a win by either player counts for both
    bool        sharedMilestones = true; // [Career] SharedMilestones - a milestone / speed trap one player completes is
                                         // completed for the other too (same rival, through the game's own award)
    bool        saveSync = true;         // [Career] SaveSync - the host keeps a copy of the client's save
    bool        bootTrace = false;       // [Debug] BootTrace - log memory-card / controller state changes while booting
    bool        perf = false;            // [Debug] Perf - log fps / frame times / mod cost every 5 s
    bool        autoPilot = false;       // [Debug] AutoPilot - the AI drives the local car in free roam and races (F5 toggles)
    bool        syncDamage = true;       // [Ghost] SyncDamage - show the other player's dents / broken lights
    bool        syncControls = true;     // [Ghost] SyncControls - show the other player's steering / lights / nitrous
    bool        autoPilotNos = true;     // [Debug] AutoPilotNos - the autopilot uses nitrous on straights
    bool        runInBackground = true;  // [Coop] RunInBackground - keep running when the window loses focus
    bool        uiIntroInput = true;  // [UI] IntroInput - menus accept input while their intro animation plays
    int         uiMenuSpeed = 2;      // [UI] MenuSpeed - 1 = normal, 2 = 2x, 3 = 3x menu animations
    std::string autoStartTarget = "freeroam"; // [AutoStart] Target - freeroam | safehouse | race
    std::string testEvent = "15.1.1"; // [Debug] TestEvent - event F7 starts
    bool        trafficSync = true;   // [Traffic] Sync
    bool        trafficSharedInRaces = true; // [Traffic] SharedInRaces - in races the client's own traffic cars follow the host's
    bool        trafficSmoothHandover = true; // [Traffic] SmoothHandover - no cars vanish / pop in when the client enters or leaves the host's traffic
    bool        trafficNoSpawnNearPlayers = true; // [Traffic] NoSpawnNearPlayers - no new traffic / cops (or copies of the other
                                          // player's cars) appear right in front of a player, within ~60 m
    int         trafficMirrorClass = 4; // [Traffic] MirrorClass - driver class for mirror cars (4 = none, 6 = remote)     // [Debug] TestKeys - F10/F11 spawn/remove a test car
    int         toggleKey = 0x77;     // [Debug] ToggleKey (VK code, default F8)
    bool        ghostEnabled = true;  // [Ghost] Enabled
    int         interpDelayMs = 100;  // [Ghost] InterpDelayMs - show the remote car this far in the past
    bool        ghostCollisions = false; // [Ghost] Collisions
    int         safeDistance = 10;    // [Ghost] SafeDistance - metres apart before collisions turn on

    void Load(const char* iniPath);
};

extern Config g_cfg;
