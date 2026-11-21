// Wire format: struct sizes and field offsets, magic, version, packet type numbers.
// Both PCs send these structs raw, so any change here is a protocol change: bump COOP_VERSION in
// src/net.h and update the numbers below in the same commit.
#include "net_helpers.h"

// ---- compile-time layout ----------------------------------------------------------------------
static_assert(sizeof(PacketHeader) == 16, "PacketHeader layout changed");
static_assert(offsetof(PacketHeader, magic) == 0 && offsetof(PacketHeader, version) == 4 &&
              offsetof(PacketHeader, type) == 6 && offsetof(PacketHeader, seq) == 8 &&
              offsetof(PacketHeader, timeMs) == 12, "PacketHeader layout changed");
static_assert(sizeof(HelloPacket) == 40, "HelloPacket layout changed");
static_assert(sizeof(PingPacket) == 20, "PingPacket layout changed");
static_assert(sizeof(CarState) == 124, "CarState layout changed");
static_assert(offsetof(CarState, pos) == 8 && offsetof(CarState, rot) == 20 && offsetof(CarState, linVel) == 36 &&
              offsetof(CarState, angVel) == 48 && offsetof(CarState, speedMps) == 60 && offsetof(CarState, simTime) == 64 &&
              offsetof(CarState, inRace) == 68 && offsetof(CarState, laps) == 70 && offsetof(CarState, pursuitHeat) == 74 &&
              offsetof(CarState, racePct) == 76 && offsetof(CarState, lapPct) == 80 && offsetof(CarState, nisHash) == 84 && offsetof(CarState, nisVote) == 88 && offsetof(CarState, eventHash) == 92 &&
              offsetof(CarState, steer) == 96 && offsetof(CarState, gas) == 100 && offsetof(CarState, nos) == 103 && offsetof(CarState, glare) == 104 &&
              offsetof(CarState, damageZones) == 108 && offsetof(CarState, brokenLights) == 112 &&
              offsetof(CarState, heat100) == 116 && offsetof(CarState, pursuitFlags) == 118 && offsetof(CarState, bustSeq) == 119 &&
              offsetof(CarState, coolLeft10) == 120, "CarState layout changed");
static_assert(sizeof(StatePacket) == 140, "StatePacket layout changed");
static_assert(CUSTOM_RECORD_SIZE == 0x198, "FECustomizationRecord size changed");
static_assert(sizeof(CarInfo) == 12 + CUSTOM_RECORD_SIZE, "CarInfo layout changed");
static_assert(sizeof(CarPacket) == 16 + 12 + CUSTOM_RECORD_SIZE, "CarPacket layout changed");
static_assert(sizeof(TrafficEntry) == 64, "TrafficEntry layout changed");
static_assert(offsetof(TrafficEntry, pos) == 12 && offsetof(TrafficEntry, rot) == 24 &&
              offsetof(TrafficEntry, linVel) == 40 && offsetof(TrafficEntry, angVel) == 52, "TrafficEntry layout changed");
static_assert(offsetof(TrafficPacket, e) == 24, "TrafficPacket layout changed");
static_assert(sizeof(TrafficPacket) == 24 + MAX_TRAFFIC_PER_PACKET * 64, "TrafficPacket layout changed");
static_assert(sizeof(EventPacket) == 60, "EventPacket layout changed");
static_assert(sizeof(RosterPacket) == 16 + 8 + 24 + CUSTOM_RECORD_SIZE, "RosterPacket layout changed");
static_assert(offsetof(RosterPacket, carKey) == 20 && offsetof(RosterPacket, name) == 24 &&
              offsetof(RosterPacket, custom) == 48, "RosterPacket layout changed");
static_assert(sizeof(RacerNamePacket) == 48, "RacerNamePacket layout changed");
static_assert(offsetof(RacerNamePacket, id) == 16 && offsetof(RacerNamePacket, name) == 18, "RacerNamePacket layout changed");
static_assert(sizeof(FileChunkPacket) == 100 + FILE_CHUNK, "FileChunkPacket layout changed");
static_assert(offsetof(FileChunkPacket, fileHash) == 16 && offsetof(FileChunkPacket, size) == 20 &&
              offsetof(FileChunkPacket, mtime) == 24 && offsetof(FileChunkPacket, index) == 32 &&
              offsetof(FileChunkPacket, total) == 34 && offsetof(FileChunkPacket, owner) == 36 &&
              offsetof(FileChunkPacket, profile) == 68 && offsetof(FileChunkPacket, data) == 100, "FileChunkPacket layout changed");
static_assert(sizeof(((FileChunkPacket*)nullptr)->owner) == 32 && sizeof(((FileChunkPacket*)nullptr)->profile) == 32, "FileChunkPacket layout changed");
static_assert(sizeof(MilestonePacket) == 32 && offsetof(MilestonePacket, msgId) == 16 && offsetof(MilestonePacket, key) == 20 &&
              offsetof(MilestonePacket, value) == 24 && offsetof(MilestonePacket, bin) == 28 && offsetof(MilestonePacket, kind) == 30,
              "MilestonePacket layout changed");
static_assert(sizeof(FileAckPacket) == 20 && offsetof(FileAckPacket, fileHash) == 16, "FileAckPacket layout changed");
static_assert(sizeof(FileReqPacket) == 48 && offsetof(FileReqPacket, owner) == 16, "FileReqPacket layout changed");
static_assert(FILE_CHUNK == 1024 && FILE_MAX == 256 * 1024, "file transfer limits changed");
// the chunk number and count travel as 16 bits
static_assert((FILE_MAX + FILE_CHUNK - 1) / FILE_CHUNK <= 0xFFFF, "a file of FILE_MAX bytes has too many chunks");

// Net::Poll reads into a 1500-byte buffer, and a datagram should fit one Ethernet frame
// (1500 MTU - 20 IP - 8 UDP) so it is never fragmented.
constexpr size_t MAX_DATAGRAM = 1472;
static_assert(sizeof(TrafficPacket) <= MAX_DATAGRAM && sizeof(CarPacket) <= MAX_DATAGRAM &&
              sizeof(RosterPacket) <= MAX_DATAGRAM && sizeof(FileChunkPacket) <= MAX_DATAGRAM, "packet does not fit one datagram");

TEST(protocol_struct_sizes) {
    // the same numbers at run time, so a failure shows up in the test report too
    CHECK_EQ(sizeof(PacketHeader), 16);
    CHECK_EQ(sizeof(HelloPacket), 40);
    CHECK_EQ(sizeof(PingPacket), 20);
    CHECK_EQ(sizeof(StatePacket), 140);
    CHECK_EQ(sizeof(CarPacket), 436);
    CHECK_EQ(sizeof(TrafficEntry), 64);
    CHECK_EQ(sizeof(TrafficPacket), 1304);
    CHECK_EQ(sizeof(EventPacket), 60);
    CHECK_EQ(sizeof(RosterPacket), 456);
    CHECK_EQ(sizeof(RacerNamePacket), 48);
    CHECK_EQ(sizeof(CarState), 124);
    CHECK_EQ(offsetof(CarState, nisHash), 84);
    CHECK_EQ(offsetof(CarState, nisVote), 88);
    CHECK_EQ(sizeof(FileChunkPacket), 1124);
    CHECK_EQ(offsetof(FileChunkPacket, data), 100);
    CHECK_EQ(sizeof(FileAckPacket), 20);
    CHECK_EQ(sizeof(FileReqPacket), 48);
    CHECK_EQ(FILE_CHUNK, 1024);
    CHECK_EQ(FILE_MAX, 262144);
    CHECK_EQ(sizeof(MilestonePacket), 32);
}

TEST(protocol_magic_and_version) {
    CHECK_EQ(COOP_MAGIC, 0x4F43574Du);
    PacketHeader h = th::Hdr(PKT_PING, 1);
    CHECK(memcmp(&h, "MWCO", 4) == 0);              // little-endian bytes on the wire
    // Pinned on purpose: if you changed a packet layout, bump COOP_VERSION and this number together.
    CHECK_EQ(COOP_VERSION, 18);
}

TEST(protocol_packet_type_numbers) {
    // never renumber: add new types at the end
    CHECK_EQ(PKT_HELLO, 1); CHECK_EQ(PKT_STATE, 2); CHECK_EQ(PKT_PING, 3); CHECK_EQ(PKT_PONG, 4);
    CHECK_EQ(PKT_BYE, 5); CHECK_EQ(PKT_CAR, 6); CHECK_EQ(PKT_TRAFFIC, 7); CHECK_EQ(PKT_EVENT, 8);
    CHECK_EQ(PKT_ROSTER, 9); CHECK_EQ(PKT_RACERNAME, 10); CHECK_EQ(PKT_RESTART, 11); CHECK_EQ(PKT_NISSKIP, 12);
    CHECK_EQ(PKT_FILE, 13); CHECK_EQ(PKT_FILEACK, 14); CHECK_EQ(PKT_FILEREQ, 15); CHECK_EQ(PKT_NOFILE, 16);
    CHECK_EQ(PKT_HIT, 17); CHECK_EQ(PKT_CARHIT, 18); CHECK_EQ(PKT_CARPART, 19); CHECK_EQ(PKT_WEATHER, 20);
    CHECK_EQ(PKT_MILESTONE, 21);       // new in this build; older builds of the same version ignore it
}

TEST(protocol_header_is_filled_by_senders) {
    th::NetFixture f(false);
    th::Connect();
    th::g_sent.clear();
    CarState s{};
    Net::SendState(s);
    Net::SendState(s);
    REQUIRE(th::g_sent.size() == 2);
    const StatePacket* a = (const StatePacket*)th::g_sent[0].data();
    const StatePacket* b = (const StatePacket*)th::g_sent[1].data();
    CHECK_EQ(th::g_sent[0].size(), sizeof(StatePacket));
    CHECK_EQ(a->h.magic, COOP_MAGIC);
    CHECK_EQ(a->h.version, COOP_VERSION);
    CHECK_EQ(a->h.type, PKT_STATE);
    CHECK_EQ(b->h.seq, a->h.seq + 1);               // sequence numbers count up
    CHECK(a->h.seq > 0);
}
