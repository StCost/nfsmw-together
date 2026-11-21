#pragma once
#include "net.h"
#include <string>

// Everything that touches speed.exe internals lives behind this interface.
namespace Game {
    bool Init();                 // verify exe version / addresses
    int  GameFlowState();        // 6 = in game (driving), -1 = unknown

    // Fill 'out' with the local player's car state. Returns false if no car (menus/loading).
    bool ReadLocalCar(CarState& out);

    // Drive the "ghost" car to the remote player's state. Called every frame while connected.
    // Returns a short status string for the overlay.
    std::string UpdateGhost(const Net::Snapshot& latest, const Net::Snapshot& prev, bool enabled);

    // Called when the peer disconnects / ghost disabled, to give the car back to the game.
    void ReleaseGhost();

    std::string DebugInfo();     // extra lines for the overlay

    bool InstallVehicleFactoryHook(); // races: build one AI racer as the peer's car
    void OnFlow(int flow);            // call every frame with the game flow state
    bool ReadLocalCarInfo(CarInfo& out); // my car model + customization

    // Traffic sync (host sends its traffic, client mirrors it when near the host)
    bool InstallTrafficHooks();
    float SimTimeSec();           // game simulation clock (s)

    // Shared event start
    bool InstallEventSync();
    bool InstallMinimapHook();     // remote players get minimap icons
    bool InstallWorldMapHook();    // ...and a world map icon
    void EventTick(bool connected);
    const char* EventStatus();
    void CoopRulesTick(bool connected);
    void PauseRuleTick(bool connected);
    std::string ToggleAutopilot();
    bool ShowChyron(const char* title, const char* line2, const char* line3);
    void QueueNotice(const std::string& text, bool warn);   // centre of the screen, orange when warn (boot.inl)
    void NoticeTick();                  // shows the queued notices (every frame)   // the game's song-title banner with our text
    void ChyronTick();                  // keeps our banner up for [UI] NoticeSeconds (every frame)
    bool CoopAllowed();                 // this career may play co-op now (career started; the prologue only with [Coop] Prologue)
    struct PrologueState { bool careerStarted; bool inPrologue; int step; };
    PrologueState PrologueNow();        // the prologue: in it?, prologue events done (cached, event.inl)
    bool InstallNoSlowMotion();
    bool InstallWeatherSync();
    void InstallFollow();               // [Coop] FollowToPursuits: come along to a Blacklist pursuit
    void FollowTick(bool linked);          // [Coop] SyncWeather: the client shows the host's rain
    void WeatherTick(bool connected);         // [Coop] NoSlowMotion: everything in real time while connected                 // this profile may play co-op (career started, prologue done)
    void SetCoopLinked(bool linked);    // co-op features on/off this frame (connected and both allowed)
    uint8_t RaceStage();                // 0 none, 1 loading / intro, 2 on the grid
    const char* StartStatus();          // "waiting for <name> to load..." while we hold the start
    uint32_t CurrentEventHash();        // event we are in, 0 = none
    std::string DebugRestart();
    std::string DebugRam(bool cop = false);   // test: hit the other player's car (cop: their nearest police car)
    std::string DebugPos();             // test: our position and speed
    std::string DebugTeleport(const char* args);   // test: 'tp x y z' puts our car there (2 m up, stopped)
    std::string DebugNisSkip();         // test: press skip on the current cutscene
    std::string DebugStopOpponents();   // test: AI opponents stop driving (so a player wins)
    std::string DebugCash();            // test: career cash and whether the current event is won
    std::string FEAccept(const char* package);   // test: send PAD_ACCEPT to a front-end screen         // test: restart the current race
    void AutopilotTick();               // [Debug] AutoPilot: AI drives in free roam / races      // test key: the game's AI drives the local car   // no pause while connected + race restart sync
    bool InstallRestartSync();
    bool InstallStreamHook();
    void InstallBootPatches();          // call from DllMain (before the front end starts)
    void BootTick();
    void PursuitResultTick();
    bool InstallNisHook();              // cutscene skip voting
    void NisTick(bool connected);
    uint32_t NisSceneHash();
    uint32_t NisVoteHash();
    // Story scenes outside events ([Coop] SyncStoryScenes, story.inl)
    uint32_t SceneHash();               // for the state packet: the story movie we watch, else NisSceneHash()
    uint8_t  CareerBin();               // career rival number (15 .. 1, 16 = prologue, 0 = no profile)
    bool InstallStoryHooks();           // skip voting on story movies
    void StoryTick(bool connected);     // the other player's story scene: wait / watch along
    bool StoryPauseWanted();            // the world must pause now (the co-op "no pause" rule steps aside)
    const char* StoryStatus();
    std::string DebugStory(const char* arg);   // test: "story" state / "story <movie>" play a movie here
    bool TakeCoopWinNotice();           // true once after a shared win was counted
    bool InstallSharedMilestones();     // [Career] SharedMilestones (milestone.inl)
    void MilestoneTick(bool linked);    // find our completions, send them, apply the other player's
    std::string TakeMilestoneNotice();  // "milestone - it counts for you too" once after one was applied here
    std::string DebugGround();                      // test: ground height / physics mode of our car and the AI cars
    std::string DebugCareerBin(const char* arg);    // test: set the current Blacklist rival (test profiles only)
    std::string DebugMilestones(const char* arg);   // test: "" list / "milestone" / "speedtrap" complete the first open one
    bool InstallCatchUp();              // AI catch-up measured against the leading human
    bool InstallCoopWin();              // shared wins
    void CoopWinTick(bool connected, const Net::Snapshot& peer);
    uint8_t LocalFinishRank(void* localRacerInfo);
    void InstallSaveSync();             // career save kept by the host
    void SaveTick(bool connected);
    const char* SaveStatus();
    void SetDataDir(const char* dir);   // scripts\ folder
    const char* NisStatus();           // [UI] SkipPursuitResults                    // [Start] DirectFreeRoam
    bool DirectStartActive();   // co-op rules (no speedbreaker)
    std::string DebugStartEvent(const char* eventId);   // F7 test
    std::string DebugListEvents();   // test command 'events': log every event ID with race type / boss / preset car
    void TrafficTick(bool connected, const Net::Snapshot& peer);
    void PursuitTick(bool connected, const Net::Snapshot& peer);   // cops may chase the other player
    int16_t LocalPursuitHeat();
    bool InstallPursuitHooks();                 // [Coop] SharedPursuits (minimap / radar) and NoPursuitCams
    void FillSharedPursuit(CarState& s);        // heat100 / pursuitFlags / bustSeq / coolLeft10 for the state packet
    int  TrafficCount();          // host: cars sent last tick; client: mirror cars alive
    bool TrafficSuppressed();     // client: own traffic switched off

    // Test: spawn a copy of the local car (same model + tuning) nearby / remove it.
    std::string DebugSpawnClone();
    std::string DebugKillClone();
    std::string DebugSpawnCop();   // F12 (TestKeys=1): spawn a patrol cop
    std::string DebugForcePursuit(); // test command 'pursuit': the cops start a pursuit on us (heat 1)
    std::string DebugKillCops();   // test command 'uncop': remove every police car (lets a pursuit end in an escape)
}
