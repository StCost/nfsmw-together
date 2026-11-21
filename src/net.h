#pragma once
#include <cstdint>
#include <string>

// Wire protocol. Little-endian, both peers are x86 so we send structs raw.
#pragma pack(push, 1)
enum PacketType : uint16_t {
    PKT_HELLO = 1,   // client -> host, host -> client (reply)
    PKT_STATE = 2,   // car state snapshot
    PKT_PING  = 3,
    PKT_PONG  = 4,
    PKT_BYE   = 5,
    PKT_CAR   = 6,   // which car the sender drives: model key + full customization (tuning, parts, paint)
    PKT_TRAFFIC = 7, // host -> client: the host's traffic cars near the client
    PKT_EVENT   = 8, // "I just started this career event" (both directions)
    PKT_ROSTER  = 9, // one AI opponent of the event the sender starts (sent before PKT_EVENT)
    PKT_RACERNAME = 10, // host -> client: display name of a race opponent (by traffic id)
    PKT_RESTART = 11,   // "I restarted the current race" (EventPacket layout)
    PKT_NISSKIP = 12,   // "I want to skip this cutscene" (EventPacket layout, eventHash = scene hash); no longer
                        // sent by the game layer: votes travel in CarState::nisHash / nisVote
    PKT_FILE    = 13,   // one chunk of a small file (career save backup / restore)
    PKT_FILEACK = 14,   // "I have the whole file with this hash"
    PKT_FILEREQ = 15,   // client -> host: "send me the save you keep for me"
    PKT_NOFILE  = 16,
    PKT_CARPART = 19,
    PKT_WEATHER = 20,   // host -> client: rain / cloud targets (older builds ignore it)   // PKT_CAR in 4 small pieces (some VPNs drop the 436-byte packet); older builds ignore it
    PKT_CARHIT  = 18,   // "my car hit YOUR traffic car / cop / racer <id> and lost this much velocity"
    PKT_HIT     = 17,   // "my car just lost this much velocity hitting yours" (momentum for the car that was hit)   // host -> client: "I keep no save for you"
    PKT_MILESTONE = 21, // "I completed this milestone / speed trap" ([Career] SharedMilestones; older builds ignore it)
};

struct PacketHeader {
    uint32_t magic;    // 'MWCO'
    uint16_t version;
    uint16_t type;
    uint32_t seq;
    uint32_t timeMs;   // sender's GetTickCount()
};

struct HelloPacket {
    PacketHeader h;
    char name[24];
};

struct PingPacket {
    PacketHeader h;
    uint32_t echoTimeMs; // for PONG: the PING's timeMs
};

struct CarState {
    int32_t  gameFlowState; // sender's game flow state (6 = in game)
    uint8_t  hasCar;        // 0 if sender is in menus / loading
    uint8_t  pad[3];        // [0] race stage (0 none, 1 loading / intro, 2 on the grid), [1] co-op allowed
                            // (past the prologue), [2] career rival number (15 .. 1; 16 prologue, 0 unknown)
    float    pos[3];
    float    rot[4];        // orientation quaternion (x,y,z,w)
    float    linVel[3];
    float    angVel[3];
    float    speedMps;
    float    simTime;       // sender's game simulation clock (s) when this state was read
    // race progress of the sender's own racer (GRacerInfo), so standings rank them correctly
    uint8_t  inRace;
    uint8_t  finished;
    int16_t  laps;
    int16_t  checkpoints;
    int16_t  pursuitHeat;   // heat x10 of the sender's pursuit, 0 = not in a pursuit
    float    racePct;
    float    lapPct;
    uint32_t nisHash;       // cutscene the sender is in (scene name hash) or story movie it watches
                            // (bStringHash of the lower-case movie name, e.g. "blacklist_05"), 0 = none
    uint32_t nisVote;       // cutscene the sender wants skipped, 0 = no vote
    uint32_t eventHash;     // event the sender is in (race parameters set), 0 = free roam
    // what the sender's car is doing, so the other PC can show it (brake lights, front wheels, smoke,
    // nitrous): the car's IInput controls and its light bits (IVehicle+0x9C)
    float    steer;         // -1 (left) .. 1 (right)
    uint8_t  gas;           // 0..255
    uint8_t  brake;         // 0..255
    uint8_t  handbrake;     // 0..255
    uint8_t  nos;           // 1 = nitrous button held
    uint32_t glare;         // light bits: head / brake / reverse / signals
    uint32_t damageZones;   // body damage: 10 zones x 3 bits (IDamageable GetZoneDamage)
    uint32_t brokenLights;  // broken head / brake lights (IDamageableVehicle +0x5C bit mask)
    // shared pursuits ([Coop] SharedPursuits; all zero when the sender has it off or isn't in free roam)
    uint16_t heat100;       // heat x100 of the sender's car (IPerpetrator::GetHeat), 0 = unknown
    uint8_t  pursuitFlags;  // PURSUIT_* bits below
    uint8_t  bustSeq;       // +1 every time the sender gets busted on its own (the other game follows)
    uint16_t coolLeft10;    // cooldown time left x10 (s) of the sender's pursuit
    uint16_t pad2;
};
enum : uint8_t {
    PURSUIT_ACTIVE   = 1,   // in a pursuit that is still running (not busted / escaped)
    PURSUIT_SEEN     = 2,   // its own cops saw the sender in the last 7 s (before any co-op hold)
    PURSUIT_COOLDOWN = 4,   // the cooldown runs
    PURSUIT_BUSTED   = 8,
    PURSUIT_KNOWN_BITS = 0x0F,
};
constexpr uint16_t MAX_HEAT100 = 1500;       // heat 15 (the career stops at 5, mods at 10)
constexpr uint16_t MAX_COOL_LEFT10 = 6000;   // 10 minutes

struct StatePacket {
    PacketHeader h;
    CarState s;
};

constexpr int CUSTOM_RECORD_SIZE = 0x198; // FECustomizationRecord (plain data: part indices, upgrades, tunings)

struct CarInfo {
    uint32_t carKey;      // vehicle model key (Attrib collection key), 0 = unknown
    uint8_t  hasCustom;
    uint8_t  pad[3];
    uint32_t customHash;  // FNV-1a of custom[], to detect changes cheaply
    uint8_t  custom[CUSTOM_RECORD_SIZE];
};

struct WeatherPacket {
    PacketHeader h;
    float rain, cloud;    // the host's rain model output (Rain::Update targets)
};
struct CarPartPacket {
    PacketHeader h;
    uint32_t id;          // carKey ^ customHash of the whole CarInfo
    uint8_t  index, count;
    uint16_t len;
    uint8_t  data[120];
};
struct CarPacket {
    PacketHeader h;
    CarInfo c;
};

struct TrafficEntry {
    uint16_t id;          // host-side id (stable while the car exists)
    uint16_t flags;       // bit0 = cop car, bit1 = race opponent, bit2 = cop in a pursuit (radar icon)
    uint32_t model;       // model key (hash of the model name)
    uint32_t glare;       // light bits (IVehicle+0x9C): head/brake/reverse/signals/cop lights
    float    pos[3];
    float    rot[4];
    float    linVel[3];
    float    angVel[3];
};
constexpr int MAX_TRAFFIC_PER_PACKET = 20;
struct EventPacket {
    PacketHeader h;
    uint32_t eventHash;   // Attrib::StringHash32 of the event ID
    char     eventId[40]; // e.g. "1.2.3"
};
struct RosterPacket {
    PacketHeader h;
    uint8_t  index, count;  // opponent index (construction order) / total AI opponents
    uint8_t  hasCustom, pad;
    uint32_t carKey;        // model key the opponent was built with
    char     name[24];      // display name (may be empty)
    uint8_t  custom[CUSTOM_RECORD_SIZE];
};
struct RacerNamePacket {
    PacketHeader h;
    uint16_t id;           // TrafficEntry id of that racer
    char     name[30];
};
constexpr int FILE_CHUNK = 1024;
constexpr uint32_t FILE_MAX = 256 * 1024;
struct FileChunkPacket {
    PacketHeader h;
    uint32_t fileHash;     // FNV-1a of the whole file
    uint32_t size;         // whole file size
    uint64_t mtime;        // the owner's file time (FILETIME, UTC)
    uint16_t index, total; // chunk number / number of chunks
    char     owner[32];    // co-op player name the save belongs to
    char     profile[32];  // game profile (save) name
    uint8_t  data[FILE_CHUNK];
};
struct HitPacket {
    PacketHeader h;
    uint32_t hitSeq;       // the copies of one hit carry the same number
    float    dv[3];        // velocity the sender's car lost in the impact (m/s, world axes)
};
struct CarHitPacket {
    PacketHeader h;
    uint32_t hitSeq;
    uint16_t id;           // TrafficEntry id of the car that was hit (owned by the receiver)
    uint16_t pad;
    float    dv[3];
};
struct MilestonePacket {
    PacketHeader h;
    uint32_t msgId;        // the copies of one completion carry the same number
    uint32_t key;          // milestone key / speed trap trigger key (game data, the same on both PCs)
    float    value;        // the sender's recorded value (milestone) / speed in m/s (speed trap)
    uint16_t bin;          // rival number the entry belongs to
    uint8_t  kind;         // 0 milestone, 1 speed trap
    uint8_t  pad;
};
struct FileAckPacket {
    PacketHeader h;
    uint32_t fileHash;
};
struct FileReqPacket {
    PacketHeader h;
    char owner[32];
};
struct TrafficPacket {
    PacketHeader h;
    uint8_t  count;
    uint8_t  pad[3];
    float    simTime;     // host simulation clock (s) for all entries
    TrafficEntry e[MAX_TRAFFIC_PER_PACKET];
};
#pragma pack(pop)

constexpr uint32_t COOP_MAGIC   = 0x4F43574D; // "MWCO"
constexpr uint16_t COOP_VERSION = 18;   // 18: shared pursuits (CarState heat100 / pursuitFlags / bustSeq / coolLeft10)

struct NetStats {
    bool        connected = false;
    std::string peerName;
    uint32_t    peerColor = 0;          // ARGB the peer wants its blip shown in (0 = the game's colour)
    std::string peerAddr;
    int         pingMs = -1;
    uint32_t    lastRecvTick = 0;
    uint32_t    connectedTick = 0;      // when this connection began
    uint32_t    peerHearsUsTick = 0;    // last ping / pong from the peer: it only pings while it hears us
    uint32_t    packetsIn = 0, packetsOut = 0;
};

// Non-blocking UDP peer. All calls happen on the game thread (from the frame hook).
namespace Net {
    bool Init();        // binds socket according to g_cfg
    void SetDataDir(const char* dir); // where to keep the remembered peer car (scripts\)
    void Shutdown();
    void SendBye();     // tell the peer we're leaving (window closing); safe to call more than once
    void Poll();        // receive everything pending, handle hello/ping
    void SendState(const CarState& s);
    void SetLocalCar(const CarInfo& c);   // remembered and re-sent periodically / on change
    const CarInfo* PeerCar();              // nullptr until the peer told us its car
    void Tick();        // periodic hello / ping / timeout handling

    // Latest snapshots from peer (newest first in [0]); valid == false if none
    struct Snapshot { bool valid = false; uint32_t recvTick = 0; uint32_t seq = 0; CarState s{}; };
    const Snapshot& Latest();
    const Snapshot& Previous();

    const NetStats& Stats();
    bool PeerSaidBye();   // the last disconnect was a clean bye (not a timeout)

    double NowMs();   // high-resolution clock (QueryPerformanceCounter), milliseconds
    double PeerClockOffsetMs();          // our clock minus the peer's (min one-way), from state packets
    void   SendTraffic(const TrafficEntry* e, int count, float simTime);
    // Local clock used to align remote motion with what this frame shows: the game's simulation
    // clock (set by the game layer); defaults to NowMs.
    extern double (*LocalClockMs)();
    // set by the game layer: called for every received traffic packet (sender time in ms)
    extern void (*OnTraffic)(const TrafficEntry* e, int count, double sendMs);
    void SendEvent(uint32_t eventHash, const char* eventId);
    extern void (*OnEvent)(uint32_t eventHash, const char* eventId);
    void SendRoster(const RosterPacket& p);                 // header is filled in here; sent 3x
    extern void (*OnRoster)(const RosterPacket& p);
    void SendRacerName(uint16_t id, const char* name);
    void SendRestart(uint32_t eventHash, uint32_t seq);
    void SendWeather(float rain, float cloud);
    bool PeerWeather(float& rain, float& cloud);              // the host's, if one came in the last 3 s      // a repeat carries the same seq
    void SendNisSkip(uint32_t sceneHash);
    void SendCarHit(uint16_t id, const float* dv);
    extern void (*OnCarHit)(uint16_t id, const float* dv);
    void SendHit(const float* dv);
    extern void (*OnHit)(const float* dv);
    // [Career] SharedMilestones: one completion (the game layer repeats it; copies share msgId)
    void SendMilestone(uint32_t msgId, uint8_t kind, uint32_t key, uint16_t bin, float value);
    extern void (*OnMilestone)(uint32_t msgId, uint8_t kind, uint32_t key, uint16_t bin, float value);
    // Small reliable file transfer (resent until acknowledged, ~10 s). One outgoing file at a time.
    // The receiver checks sizes, chunk numbers and the hash; the names are passed on as received
    // (NUL-terminated, up to 32 characters) and are NOT safe as file names: see saveutil.h.
    // A client takes file packets only from the host's address.
    bool SendFile(const char* owner, const char* profile, const void* data, uint32_t size, uint64_t mtime);
    bool SendingFile();
    bool LastFileDelivered();                      // the last SendFile ended with the peer's acknowledgement
    void RequestFile(const char* owner);           // client: ask the host for the save it keeps for us
    void SendNoFile();
    extern void (*OnFile)(const char* owner, const char* profile, const uint8_t* data, uint32_t size, uint64_t mtime);
    extern void (*OnFileRequest)(const char* owner);
    extern void (*OnNoFile)();
    extern void (*OnNisSkip)(uint32_t sceneHash);
    extern void (*OnRestart)(uint32_t eventHash, uint32_t seq);
    extern void (*OnRacerName)(uint16_t id, const char* name);

    // Snapshot interpolation: state of the remote car as it was `delayMs` ago (in sender time),
    // blended between the two surrounding packets (Hermite on position using velocities,
    // normalized-lerp on rotation). Extrapolates up to 250 ms when packets are late.
    // Returns false if there is no data. *extrapolatedMs = how far past the newest packet we are.
    bool SampleRemote(double delayMs, CarState& out, double* extrapolatedMs = nullptr);
    // The other player's race % now (their newest one moved on by their recent rate, see
    // NetUtil::RacePctAt; `aheadMs` = how far past our clock mapping to look, e.g. half the ping).
    // False if there is no data.
    bool RemoteRacePctNow(double aheadMs, float& pct);
    const char* LastError();

#ifdef MWT_TESTS
    // Test hooks (tests\, built with /DMWT_TESTS; not part of the plugin).
    void Test_Reset();                      // every global back to its start-up value, socket closed
    void Test_HandlePacket(const void* buf, int len, uint32_t fromIp, uint16_t fromPort); // as if received (host byte order)
    void Test_SetTick(bool on, uint32_t ms = 0);          // replace GetTickCount() inside net.cpp
    void Test_SetCapture(void (*fn)(const void* data, int len)); // receive what would be sent (no socket needed)
    void Test_SetPeer(uint32_t ip, uint16_t port);        // what Init does for a client (host byte order)
    struct TestState { bool havePeer; uint32_t peerIp; uint16_t peerPort; uint32_t lastStateSeq; int ringCount; bool leaving; bool socketOpen; };
    TestState Test_GetState();
#endif
}
