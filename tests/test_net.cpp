// Net: connection handling, sequence numbers, snapshot ring / interpolation, clock offset,
// bye handling, timeouts and the periodic sends. No socket: packets go in through
// Net::Test_HandlePacket and come out through the capture hook.
#include "net_helpers.h"
#include <limits>

using namespace th;

// ---- connecting -------------------------------------------------------------------------------
TEST(net_host_connects_on_hello_and_replies) {
    NetFixture f(true);
    CHECK(!Net::Stats().connected);
    Inject(MakeHello("Alice"));
    CHECK(Net::Stats().connected);
    CHECK_STR(Net::Stats().peerName, "Alice");
    CHECK_STR(Net::Stats().peerAddr, "10.0.0.2:47555");
    CHECK_EQ(Net::Stats().packetsIn, 1);
    Net::TestState st = Net::Test_GetState();
    CHECK(st.havePeer);
    CHECK_EQ(st.peerIp, PEER_IP);
    // the host answers with its own hello so the client learns its name
    REQUIRE(SentOfType(PKT_HELLO) == 1);
    CHECK_EQ(g_sent[0].size(), sizeof(HelloPacket));
    CHECK_STR(FirstSent<HelloPacket>(PKT_HELLO)->name, "Tester");
}

TEST(net_client_does_not_echo_hello) {
    NetFixture f(false);
    Inject(MakeHello("Host"));
    CHECK(Net::Stats().connected);
    CHECK_STR(Net::Stats().peerName, "Host");
    CHECK_EQ(SentOfType(PKT_HELLO), 0);              // otherwise the two sides would ping-pong hellos
}

TEST(net_hello_name_limits) {
    {   // our own name: 15 characters (the HUD limit); the last 4 bytes of the field carry the instance id
        NetFixture f(false);
        g_cfg.playerName = "0123456789012345678901234567890123456789";
        Net::Tick();
        REQUIRE(SentOfType(PKT_HELLO) == 1);
        const HelloPacket* h = FirstSent<HelloPacket>(PKT_HELLO);
        CHECK_EQ(h->name[15], 0);
        CHECK_STR(std::string(h->name, strnlen(h->name, 24)), "012345678901234");
        uint32_t id = 0; memcpy(&id, h->name + 20, 4);
        CHECK(id != 0);                                  // our instance id, so our own hello can be recognised
    }
    {   // a peer name that fills the field without a terminator must not be read past its end
        NetFixture f(false);
        HelloPacket p{};
        p.h = Hdr(PKT_HELLO, 1);
        memset(p.name, 'A', sizeof(p.name));
        Inject(p);
        CHECK_EQ(Net::Stats().peerName.size(), 16);           // bytes 16.. carry the colour and the instance id
    }
}

TEST(net_rejects_foreign_and_malformed_packets) {
    NetFixture f(true);
    HelloPacket p = MakeHello("X");

    InjectRaw(&p, 0);
    InjectRaw(&p, 15);                               // shorter than a header
    CHECK(!Net::Stats().connected);

    HelloPacket bad = p; bad.h.magic = 0x12345678;
    Inject(bad);
    CHECK(!Net::Stats().connected);

    bad = p; bad.h.version = COOP_VERSION + 1;       // a different mod version: refuse, and say why
    Inject(bad);
    CHECK(!Net::Stats().connected);
    CHECK(strstr(Net::LastError(), "protocol") != nullptr);
    CHECK_EQ(Net::Stats().packetsIn, 0);
    CHECK(g_sent.empty());

    // unknown packet type from a matching version: counted, otherwise ignored
    PacketHeader unk = Hdr(999, 1);
    Inject(unk);
    CHECK_EQ(Net::Stats().packetsIn, 1);
}

TEST(net_truncated_packets_are_dropped) {
    NetFixture f(false);
    Connect();
    int events = 0;
    static int* s_events; s_events = &events;
    Net::OnEvent = [](uint32_t, const char*) { ++*s_events; };
    Net::OnRestart = [](uint32_t, uint32_t) { ++*s_events; };
    Net::OnRoster = [](const RosterPacket&) { ++*s_events; };
    Net::OnRacerName = [](uint16_t, const char*) { ++*s_events; };
    Net::OnTraffic = [](const TrafficEntry*, int, double) { ++*s_events; };

    StatePacket st = MakeState(5, 1.0f, 0, 0);
    InjectRaw(&st, sizeof(st) - 1);
    CHECK(!Net::Latest().valid);
    EventPacket ev{}; ev.h = Hdr(PKT_EVENT, 6);
    InjectRaw(&ev, sizeof(ev) - 1);
    ev.h.type = PKT_RESTART;
    InjectRaw(&ev, sizeof(ev) - 1);
    RosterPacket ro{}; ro.h = Hdr(PKT_ROSTER, 7);
    InjectRaw(&ro, sizeof(ro) - 1);
    RacerNamePacket rn{}; rn.h = Hdr(PKT_RACERNAME, 8);
    InjectRaw(&rn, sizeof(rn) - 1);
    CarPacket car{}; car.h = Hdr(PKT_CAR, 9); car.c.carKey = 0xABC;
    InjectRaw(&car, sizeof(car) - 1);
    CHECK(Net::PeerCar() == nullptr);
    TrafficPacket tr{}; tr.h = Hdr(PKT_TRAFFIC, 10);
    InjectRaw(&tr, (int)offsetof(TrafficPacket, e) - 1);
    PingPacket pi{}; pi.h = Hdr(PKT_PING, 11);
    g_sent.clear();
    InjectRaw(&pi, sizeof(pi) - 1);
    CHECK(g_sent.empty());                           // no pong for half a ping
    CHECK_EQ(events, 0);
}

TEST(net_host_ignores_third_parties) {
    NetFixture f(true);
    Connect("Alice");
    uint32_t in = Net::Stats().packetsIn;

    Inject(MakeState(10, 1.0f, 5, 0), OTHER_IP);     // someone else on the LAN
    Inject(MakeHello("Mallory"), OTHER_IP);
    PacketHeader bye = Hdr(PKT_BYE, 3);
    Inject(bye, OTHER_IP);                           // must not be able to kick the real peer
    Inject(MakeHello("Mallory"), PEER_IP, 1234);     // same address, other port: also a stranger
    CHECK_EQ(Net::Stats().packetsIn, in);
    CHECK(Net::Stats().connected);
    CHECK_STR(Net::Stats().peerName, "Alice");
    CHECK(!Net::Latest().valid);
    CHECK_EQ(Net::Test_GetState().peerIp, PEER_IP);
}

TEST(net_host_accepts_new_peer_when_old_one_is_stale) {
    NetFixture f(true);
    Connect("Alice");
    f.Advance(15001);                                // nothing heard for longer than the timeout
    Inject(MakeHello("Bob"), OTHER_IP, 5000);
    CHECK_EQ(Net::Test_GetState().peerIp, OTHER_IP);
    CHECK_EQ(Net::Test_GetState().peerPort, 5000);
    CHECK_STR(Net::Stats().peerName, "Bob");
}

// ---- state packets ----------------------------------------------------------------------------
TEST(net_state_sequence_duplicates_and_reordering) {
    NetFixture f(false);
    Connect();
    Inject(MakeState(10, 1.00f, 100, 0));
    CHECK(Net::Latest().valid);
    CHECK_EQ(Net::Latest().seq, 10);
    CHECK(!Net::Previous().valid);

    Inject(MakeState(9, 0.98f, 999, 0));             // late
    Inject(MakeState(10, 1.00f, 999, 0));            // duplicate
    CHECK_EQ(Net::Latest().seq, 10);
    CHECK_NEAR(Net::Latest().s.pos[0], 100, 1e-3);
    CHECK_EQ(Net::Test_GetState().ringCount, 1);

    Inject(MakeState(12, 1.03f, 103, 0));            // a gap (11 lost) is fine
    CHECK_EQ(Net::Latest().seq, 12);
    CHECK_EQ(Net::Previous().seq, 10);
    CHECK_EQ(Net::Test_GetState().ringCount, 2);

    Inject(MakeState(11, 1.02f, 999, 0));            // 11 turns up after 12: dropped
    CHECK_EQ(Net::Latest().seq, 12);
    CHECK_EQ(Net::Test_GetState().ringCount, 2);
    CHECK_EQ(Net::Latest().recvTick, T0);
}

TEST(net_state_peer_restart_is_detected_by_sequence) {
    NetFixture f(false);
    Connect();
    Inject(MakeState(1000, 50.0f, 100, 0));
    Inject(MakeState(800, 50.1f, 999, 0));           // 200 behind: just a very late packet
    CHECK_EQ(Net::Latest().seq, 1000);
    Inject(MakeState(2, 0.5f, 7, 0));                // far behind: the peer started again from 1
    CHECK_EQ(Net::Latest().seq, 2);
    CHECK_NEAR(Net::Latest().s.pos[0], 7, 1e-3);
    CHECK_EQ(Net::Test_GetState().lastStateSeq, 2);
    // its simulation clock jumped too, so the interpolation history starts over
    CHECK_EQ(Net::Test_GetState().ringCount, 1);
}

TEST(net_state_history_resets_on_sim_clock_jump) {
    NetFixture f(false);
    Connect();
    Inject(MakeState(1, 100.0f, 0, 0));
    Inject(MakeState(2, 100.1f, 0, 0));
    Inject(MakeState(3, 100.2f, 0, 0));
    CHECK_EQ(Net::Test_GetState().ringCount, 3);
    Inject(MakeState(4, 110.0f, 0, 0));              // +9.8 s: the peer loaded another world
    CHECK_EQ(Net::Test_GetState().ringCount, 1);
    Inject(MakeState(5, 2.0f, 0, 0));                // back near zero: same
    CHECK_EQ(Net::Test_GetState().ringCount, 1);
    Inject(MakeState(6, 6.9f, 0, 0));                // 4.9 s apart: still the same history
    CHECK_EQ(Net::Test_GetState().ringCount, 2);
}

TEST(net_state_ring_holds_32_and_wraps) {
    NetFixture f(false);
    Connect();
    for (int i = 0; i < 100; ++i) {
        g_clockMs = 5000 + i * 16;
        Inject(MakeState(1 + i, 1.0f + i * 0.016f, (float)i, 0));
    }
    CHECK_EQ(Net::Test_GetState().ringCount, 32);
    CarState out{};
    g_clockMs = 5000 + 99 * 16;
    REQUIRE(Net::SampleRemote(0, out));
    CHECK_NEAR(out.pos[0], 99, 1e-2);                // the newest sample survived the wrap
}

TEST(net_state_with_bad_numbers_is_dropped) {
    NetFixture f(false);
    Connect();
    Inject(MakeState(1, 1.0f, 100, 10));
    StatePacket bad = MakeState(2, 1.1f, 101, 10);
    bad.s.pos[1] = std::numeric_limits<float>::quiet_NaN();
    Inject(bad);
    bad = MakeState(3, 1.1f, 101, 10);
    bad.s.linVel[0] = std::numeric_limits<float>::infinity();
    Inject(bad);
    bad = MakeState(4, std::numeric_limits<float>::quiet_NaN(), 101, 10);
    Inject(bad);
    CHECK_EQ(Net::Latest().seq, 1);
    CHECK_EQ(Net::Test_GetState().ringCount, 1);
    CHECK_EQ(Net::Test_GetState().lastStateSeq, 1);  // a dropped packet doesn't block the next good one
    Inject(MakeState(2, 1.1f, 101, 10));
    CHECK_EQ(Net::Latest().seq, 2);

    // "not driving" (all zeros, zero quaternion) is a normal state and must get through
    StatePacket idle{}; idle.h = Hdr(PKT_STATE, 3); idle.s.gameFlowState = 3;
    Inject(idle);
    CHECK_EQ(Net::Latest().seq, 3);
    CHECK_EQ(Net::Latest().s.hasCar, 0);
}

// ---- interpolation ----------------------------------------------------------------------------
// The peer drives along +x at 10 m/s; its simulation clock reads 1.0 s when ours reads 5000 ms.
static void FeedThreeSamples() {
    g_clockMs = 5000; Inject(MakeState(1, 1.0f, 10.0f, 10.0f));
    g_clockMs = 5100; Inject(MakeState(2, 1.1f, 11.0f, 10.0f));
    g_clockMs = 5200; Inject(MakeState(3, 1.2f, 12.0f, 10.0f));
}

TEST(net_sample_without_data_fails) {
    NetFixture f(false);
    CarState out{};
    CHECK(!Net::SampleRemote(100, out));
    Connect();
    CHECK(!Net::SampleRemote(100, out));
    CHECK_NEAR(Net::PeerClockOffsetMs(), 0, 1e-9);
}

TEST(net_sample_interpolates_between_snapshots) {
    NetFixture f(false);
    Connect();
    FeedThreeSamples();
    CHECK_NEAR(Net::PeerClockOffsetMs(), 4000, 0.01);

    CarState out{}; double ex = -1;
    g_clockMs = 5250;                                // 100 ms in the past = peer time 1150 ms
    REQUIRE(Net::SampleRemote(100, out, &ex));
    CHECK_NEAR(out.pos[0], 11.5, 1e-3);
    CHECK_NEAR(out.linVel[0], 10, 1e-4);
    CHECK_NEAR(out.speedMps, 10, 1e-4);
    CHECK_NEAR(out.rot[3], 1, 1e-5);
    CHECK_NEAR(ex, 0, 1e-9);
    CHECK_EQ(out.hasCar, 1);

    REQUIRE(Net::SampleRemote(175, out, &ex));       // peer time 1075: between the first two
    CHECK_NEAR(out.pos[0], 10.75, 1e-3);

    REQUIRE(Net::SampleRemote(50, out, &ex));        // exactly on the newest snapshot
    CHECK_NEAR(out.pos[0], 12.0, 1e-3);
    CHECK_NEAR(ex, 0, 1e-2);
}

TEST(net_sample_interpolates_velocity_and_rotation) {
    NetFixture f(false);
    Connect();
    const float s = 0.70710678f;
    StatePacket a = MakeState(1, 1.0f, 0, 0), b = MakeState(2, 1.2f, 0, 20);
    b.s.rot[2] = s; b.s.rot[3] = s;                  // turned 90 degrees about z
    b.s.angVel[2] = 4.0f;
    g_clockMs = 1000; Inject(a);
    g_clockMs = 1200; Inject(b);
    CarState out{};
    g_clockMs = 1100;
    REQUIRE(Net::SampleRemote(0, out));
    CHECK_NEAR(out.linVel[0], 10, 1e-3);
    CHECK_NEAR(out.angVel[2], 2, 1e-3);
    CHECK_NEAR(out.speedMps, 10, 1e-3);
    CHECK_NEAR(out.rot[2], sinf(3.14159265f / 8), 1e-3);
    CHECK_NEAR(out.rot[3], cosf(3.14159265f / 8), 1e-3);
    // accelerating from rest to 20 m/s without moving the endpoints: Hermite bulges backwards
    CHECK_NEAR(out.pos[0], NetUtil::Hermite(0, 0, 0, 20, 0.2f, 0.5f), 1e-4);
}

TEST(net_sample_extrapolates_at_most_250ms) {
    NetFixture f(false);
    Connect();
    FeedThreeSamples();
    CarState out{}; double ex = 0;

    g_clockMs = 5400;                                // 200 ms after the newest packet, no delay
    REQUIRE(Net::SampleRemote(0, out, &ex));
    CHECK_NEAR(ex, 200, 0.05);
    CHECK_NEAR(out.pos[0], 14.0, 1e-2);              // 12 m + 10 m/s * 0.2 s

    g_clockMs = 5450;
    REQUIRE(Net::SampleRemote(0, out, &ex));
    CHECK_NEAR(ex, 250, 0.05);
    CHECK_NEAR(out.pos[0], 14.5, 1e-2);

    g_clockMs = 9000;                                // packets stopped: the car must not run away
    REQUIRE(Net::SampleRemote(0, out, &ex));
    CHECK_NEAR(ex, 250, 1e-9);
    CHECK_NEAR(out.pos[0], 14.5, 1e-2);
}

TEST(net_sample_before_history_uses_newest) {
    NetFixture f(false);
    Connect();
    FeedThreeSamples();
    CarState out{}; double ex = -1;
    g_clockMs = 5200;
    REQUIRE(Net::SampleRemote(2000, out, &ex));      // asked for a time older than everything we have
    CHECK_NEAR(out.pos[0], 12.0, 1e-3);
    CHECK_NEAR(ex, 0, 1e-9);
}

TEST(net_sample_single_snapshot) {
    NetFixture f(false);
    Connect();
    g_clockMs = 5000; Inject(MakeState(1, 1.0f, 10.0f, 10.0f));
    CarState out{}; double ex = 0;
    g_clockMs = 5100;
    REQUIRE(Net::SampleRemote(0, out, &ex));
    CHECK_NEAR(out.pos[0], 11.0, 1e-2);
    CHECK_NEAR(ex, 100, 0.05);
    REQUIRE(Net::SampleRemote(100, out, &ex));
    CHECK_NEAR(out.pos[0], 10.0, 1e-2);
}

TEST(net_sample_ignores_duplicate_sim_time) {
    // two packets with the same simulation time (the peer's game was paused): no division by zero
    NetFixture f(false);
    Connect();
    g_clockMs = 5000; Inject(MakeState(1, 1.0f, 10.0f, 0.0f));
    g_clockMs = 5016; Inject(MakeState(2, 1.0f, 10.0f, 0.0f));
    g_clockMs = 5032; Inject(MakeState(3, 1.0f, 10.0f, 0.0f));
    CarState out{};
    for (double d : { 0.0, 16.0, 32.0, 100.0 }) {
        REQUIRE(Net::SampleRemote(d, out));
        CHECK(AllFinite(out));
        CHECK_NEAR(out.pos[0], 10.0, 1e-3);
    }
}

// ---- clock offset -----------------------------------------------------------------------------
TEST(net_clock_offset_glides_and_snaps) {
    NetFixture f(false);
    Connect();
    g_clockMs = 5000; Inject(MakeState(1, 1.0f, 0, 0));
    CHECK_NEAR(Net::PeerClockOffsetMs(), 4000, 0.01);   // first estimate: taken as is

    // a faster packet arrives: the raw estimate (the minimum) drops by 10 ms...
    g_clockMs = 5990; Inject(MakeState(2, 2.0f, 0, 0));
    g_clockMs = 6000;
    // ...but the estimate may only move 2 ms per second, or remote cars would jump
    CHECK_NEAR(Net::PeerClockOffsetMs(), 3998, 0.01);
    g_clockMs = 6500;
    CHECK_NEAR(Net::PeerClockOffsetMs(), 3997, 0.01);
    CHECK_NEAR(Net::PeerClockOffsetMs(), 3997, 0.01);   // no time passed: no movement
    g_clockMs = 60000;
    CHECK_NEAR(Net::PeerClockOffsetMs(), 3990, 0.01);   // gets there, and stops there
    g_clockMs = 70000;
    CHECK_NEAR(Net::PeerClockOffsetMs(), 3990, 0.01);

    // more than 50 ms off: the peer restarted, snap at once
    g_clockMs = 70010; Inject(MakeState(3, 66.5f, 0, 0));  // new history; recv - send = 3510
    CHECK_EQ(Net::Test_GetState().ringCount, 1);
    CHECK_NEAR(Net::PeerClockOffsetMs(), 3510, 0.01);
}

TEST(net_clock_offset_glides_upwards_too) {
    NetFixture f(false);
    Connect();
    g_clockMs = 5000; Inject(MakeState(1, 1.0f, 0, 0));
    CHECK_NEAR(Net::PeerClockOffsetMs(), 4000, 0.01);
    g_clockMs = 20000;
    CHECK_NEAR(Net::PeerClockOffsetMs(), 4000, 0.01);
    // the peer's clock jumps (new history) so that the raw estimate is 20 ms higher
    g_clockMs = 20020; Inject(MakeState(2, 16.0f, 0, 0));
    CHECK_EQ(Net::Test_GetState().ringCount, 1);
    g_clockMs = 21000;
    CHECK_NEAR(Net::PeerClockOffsetMs(), 4002, 0.01);
    g_clockMs = 22000;
    CHECK_NEAR(Net::PeerClockOffsetMs(), 4004, 0.01);
    g_clockMs = 19000;                               // the local clock restarted: no step backwards
    CHECK_NEAR(Net::PeerClockOffsetMs(), 4004, 0.01);
}

TEST(net_clock_offset_kept_while_history_is_empty) {
    NetFixture f(false);
    Connect();
    g_clockMs = 5000; Inject(MakeState(1, 1.0f, 0, 0));
    CHECK_NEAR(Net::PeerClockOffsetMs(), 4000, 0.01);
    PacketHeader bye = Hdr(PKT_BYE, 2);
    Inject(bye);
    f.Advance(3000);
    Connect();                                       // reconnect: history cleared
    CHECK_EQ(Net::Test_GetState().ringCount, 0);
    CHECK_NEAR(Net::PeerClockOffsetMs(), 4000, 0.01);
}

// ---- bye --------------------------------------------------------------------------------------
TEST(net_bye_disconnects_and_ignores_leftovers_for_2s) {
    NetFixture f(true);
    Connect("Alice");
    Inject(MakeState(5, 1.0f, 1, 0));
    CHECK(Net::Latest().valid);
    CHECK(!Net::PeerSaidBye());

    PacketHeader bye = Hdr(PKT_BYE, 6);
    Inject(bye);
    CHECK(!Net::Stats().connected);
    CHECK(Net::PeerSaidBye());
    CHECK(!Net::Latest().valid);
    CHECK(!Net::Previous().valid);
    CHECK(!Net::Test_GetState().havePeer);           // the host forgets the address

    // the leaving game's last packets and the two extra byes must not reconnect us
    uint32_t in = Net::Stats().packetsIn;
    Inject(bye); Inject(bye);
    f.Advance(500);
    Inject(MakeState(7, 1.1f, 2, 0));
    PingPacket ping{}; ping.h = Hdr(PKT_PING, 8);
    g_sent.clear();
    Inject(ping);
    f.Advance(1499);                                 // 1999 ms after the bye
    Inject(MakeState(9, 1.2f, 3, 0));
    CHECK(!Net::Stats().connected);
    CHECK_EQ(Net::Stats().packetsIn, in);
    CHECK(g_sent.empty());
    CHECK(Net::PeerSaidBye());

    f.Advance(1);                                    // 2000 ms: the window is over
    Inject(MakeState(10, 1.3f, 4, 0));
    CHECK(Net::Stats().connected);
    CHECK(!Net::PeerSaidBye());
}

TEST(net_hello_right_after_bye_reconnects) {
    NetFixture f(true);
    Connect("Alice");
    Inject(MakeState(500, 30.0f, 1, 0));
    PacketHeader bye = Hdr(PKT_BYE, 501);
    Inject(bye);
    f.Advance(300);                                  // the game was started again straight away
    Inject(MakeHello("Alice", 1));
    CHECK(Net::Stats().connected);
    CHECK(!Net::PeerSaidBye());
    // still inside the 2 s window: only the hello counted, state packets wait
    Inject(MakeState(2, 0.5f, 9, 0));
    CHECK(!Net::Latest().valid);
    f.Advance(1700);
    Inject(MakeState(3, 0.6f, 9, 0));                // sequence numbers started over: accepted
    CHECK(Net::Latest().valid);
    CHECK_EQ(Net::Latest().seq, 3);
}

TEST(net_bye_window_survives_tick_wraparound) {
    NetFixture f(false);
    f.SetTick(0xFFFFFF00u);
    Connect();
    PacketHeader bye = Hdr(PKT_BYE, 2);
    Inject(bye);
    f.SetTick(0x00000100u);                          // 512 ms later, GetTickCount wrapped
    Inject(MakeState(3, 1.0f, 0, 0));
    CHECK(!Net::Stats().connected);
    f.SetTick(0x00000100u + 1600);
    Inject(MakeState(4, 1.0f, 0, 0));
    CHECK(Net::Stats().connected);
}

TEST(net_bye_at_tick_zero_still_opens_the_window) {
    NetFixture f(false);
    f.SetTick(0);
    Connect();
    PacketHeader bye = Hdr(PKT_BYE, 2);
    Inject(bye);
    f.SetTick(100);
    Inject(MakeState(3, 1.0f, 0, 0));
    CHECK(!Net::Stats().connected);
}

TEST(net_client_keeps_host_address_after_bye) {
    NetFixture f(false);
    Connect("Host");
    PacketHeader bye = Hdr(PKT_BYE, 2);
    Inject(bye);
    CHECK(!Net::Stats().connected);
    CHECK(Net::Test_GetState().havePeer);            // keeps saying hello to the configured host
    g_sent.clear();
    f.Advance(1500);
    Net::Tick();
    CHECK_EQ(SentOfType(PKT_HELLO), 1);
}

TEST(net_send_bye_goes_out_three_times_then_silence) {
    NetFixture f(false);
    Connect();
    g_sent.clear();
    Net::SendBye();
    CHECK_EQ(SentOfType(PKT_BYE), 3);
    CHECK_EQ(g_sent.size(), 3);
    for (auto& p : g_sent) CHECK_EQ(p.size(), sizeof(PacketHeader));
    CHECK(Net::Test_GetState().leaving);

    // after saying bye we send nothing: any packet would make the peer think we came back
    g_sent.clear();
    Net::SendBye();
    CarState s{};
    Net::SendState(s);
    Net::SendEvent(1, "1.2.3");
    f.Advance(5000);
    Net::Tick();
    PingPacket ping{}; ping.h = Hdr(PKT_PING, 9);
    Inject(ping);
    CHECK(g_sent.empty());
}

// ---- timeouts and periodic sends --------------------------------------------------------------
TEST(net_peer_times_out_after_15s) {
    NetFixture f(false);
    Connect();
    Net::Tick();
    for (int i = 0; i < 30; ++i) {                   // 15.0 s of silence: not yet
        f.Advance(500);
        Net::Tick();
    }
    CHECK(Net::Stats().connected);
    f.Advance(500);
    Net::Tick();
    CHECK(!Net::Stats().connected);
    CHECK(!Net::PeerSaidBye());                      // a timeout, not a clean leave
    CHECK_EQ(Net::Stats().pingMs, -1);
    CHECK(!Net::Latest().valid);
}

TEST(net_own_stall_is_not_a_peer_timeout) {
    // our frame loop stood still for 30 s (loading screen): the peer's packets are still queued
    NetFixture f(false);
    Connect();
    Net::Tick();
    f.Advance(30000);
    Net::Tick();
    CHECK(Net::Stats().connected);
    for (int i = 0; i < 31; ++i) { f.Advance(500); Net::Tick(); }   // but then real silence counts
    CHECK(!Net::Stats().connected);
}

TEST(net_packets_keep_connection_alive) {
    NetFixture f(false);
    Connect();
    for (int i = 0; i < 100; ++i) {
        f.Advance(500);
        PingPacket ping{}; ping.h = Hdr(PKT_PING, 10 + i);
        Inject(ping);
        Net::Tick();
    }
    CHECK(Net::Stats().connected);
}

TEST(net_client_hello_and_ping_cadence) {
    NetFixture f(false);
    for (int i = 0; i <= 30; ++i) { f.SetTick(T0 + i * 100); Net::Tick(); }   // 3 s, not connected
    CHECK(SentOfType(PKT_HELLO) >= 3 && SentOfType(PKT_HELLO) <= 4);          // about one per second
    CHECK_EQ(SentOfType(PKT_PING), 0);

    Connect();
    g_sent.clear();
    for (int i = 0; i <= 90; ++i) {                  // 9 s connected, kept alive by the peer
        f.Advance(100);
        if (i % 10 == 0) { PingPacket p{}; p.h = Hdr(PKT_PONG, 100 + i); p.echoTimeMs = 0; Inject(p); }
        Net::Tick();
    }
    CHECK(SentOfType(PKT_HELLO) >= 2 && SentOfType(PKT_HELLO) <= 4);          // every 3 s
    CHECK(SentOfType(PKT_PING) >= 8 && SentOfType(PKT_PING) <= 10);           // every second
}

TEST(net_host_is_quiet_until_someone_connects) {
    NetFixture f(true);
    for (int i = 0; i < 50; ++i) { f.Advance(100); Net::Tick(); }
    CarState s{};
    Net::SendState(s);
    Net::SendEvent(1, "1.2.3");
    Net::SendRestart(1, 1);
    Net::SendRacerName(1, "x");
    RosterPacket r{};
    Net::SendRoster(r);
    TrafficEntry e = MakeTrafficEntry(1, 0);
    Net::SendTraffic(&e, 1, 1.0f);
    CHECK(g_sent.empty());
    CHECK_EQ(Net::Stats().packetsOut, 0);
}

TEST(net_host_calls_configured_client) {
    // [Network] ClientIP set: the host says hello first (opens both firewalls)
    NetFixture f(true);
    Net::Test_SetPeer(PEER_IP, PEER_PORT);
    for (int i = 0; i < 25; ++i) { f.Advance(100); Net::Tick(); }
    CHECK(SentOfType(PKT_HELLO) >= 2);
    Connect();
    g_sent.clear();
    for (int i = 0; i < 50; ++i) {
        f.Advance(100);
        PingPacket p{}; p.h = Hdr(PKT_PING, 50 + i); Inject(p);
        Net::Tick();
    }
    CHECK_EQ(SentOfType(PKT_HELLO), 0);              // connected: the host stops calling
}

TEST(net_client_sends_state_before_connection) {
    NetFixture f(false);
    CarState s{};
    Net::SendState(s);
    CHECK_EQ(SentOfType(PKT_STATE), 1);              // also punches the client's firewall
}

// ---- ping -------------------------------------------------------------------------------------
TEST(net_ping_is_answered_with_echo) {
    NetFixture f(false);
    Connect();
    g_sent.clear();
    PingPacket ping{}; ping.h = Hdr(PKT_PING, 0, 0xDEADBEEF);   // seq 0: what the keep-alive thread sends
    Inject(ping);
    REQUIRE(g_sent.size() == 1);
    const PingPacket* pong = FirstSent<PingPacket>(PKT_PONG);
    REQUIRE(pong != nullptr);
    CHECK_EQ(g_sent[0].size(), sizeof(PingPacket));
    CHECK_EQ(pong->echoTimeMs, 0xDEADBEEF);
}

TEST(net_pong_measures_round_trip) {
    NetFixture f(false);
    Connect();
    CHECK_EQ(Net::Stats().pingMs, -1);
    uint32_t now = (uint32_t)(uint64_t)Net::NowMs();
    PingPacket pong{}; pong.h = Hdr(PKT_PONG, 2); pong.echoTimeMs = now - 40;
    Inject(pong);
    CHECK(Net::Stats().pingMs >= 40 && Net::Stats().pingMs < 1000);

    // a pong that echoes a time we never sent (from the future / ancient) must not wreck the display
    int before = Net::Stats().pingMs;
    pong.h.seq = 3; pong.echoTimeMs = now + 5000000;
    Inject(pong);
    pong.h.seq = 4; pong.echoTimeMs = now - 100000000;
    Inject(pong);
    CHECK_EQ(Net::Stats().pingMs, before);
}

// ---- car info ---------------------------------------------------------------------------------
static CarInfo MakeCar(uint32_t key, uint8_t fill) {
    CarInfo c{};
    c.carKey = key; c.hasCustom = 1; c.customHash = 0x1000u + fill;
    memset(c.custom, fill, sizeof(c.custom));
    return c;
}

TEST(net_local_car_is_sent_on_change_and_every_2s) {
    NetFixture f(false);
    Net::SetLocalCar(MakeCar(0xAAAA, 1));            // not connected yet: remembered only
    CHECK_EQ(SentOfType(PKT_CAR), 0);
    Connect();
    f.Advance(2001);
    Net::Tick();
    REQUIRE(SentOfType(PKT_CAR) == 1);
    const CarPacket* cp = FirstSent<CarPacket>(PKT_CAR);
    CHECK_EQ(cp->c.carKey, 0xAAAA);
    CHECK_EQ(cp->c.custom[CUSTOM_RECORD_SIZE - 1], 1);

    g_sent.clear();
    Net::SetLocalCar(MakeCar(0xAAAA, 1));            // unchanged: nothing extra
    CHECK_EQ(SentOfType(PKT_CAR), 0);
    Net::SetLocalCar(MakeCar(0xBBBB, 2));            // changed car: at once
    CHECK_EQ(SentOfType(PKT_CAR), 1);
    Net::SetLocalCar(MakeCar(0xBBBB, 3));            // same car, new tuning: at once
    CHECK_EQ(SentOfType(PKT_CAR), 2);

    g_sent.clear();
    for (int i = 0; i < 10; ++i) {
        f.Advance(500);
        PingPacket p{}; p.h = Hdr(PKT_PING, 50 + i); Inject(p);
        Net::Tick();
    }
    CHECK_EQ(SentOfType(PKT_CAR), 2);                // 5 s: resent twice (UDP may drop it)
}

TEST(net_peer_car_is_stored_and_survives_restart) {
    std::string dir = TempDir();
    std::string file = dir + "MWTogether.peercar.bin";
    DeleteFileA(file.c_str());
    {
        NetFixture f(false);
        Net::SetDataDir(dir.c_str());
        CHECK(Net::PeerCar() == nullptr);
        Connect();
        CarPacket p{}; p.h = Hdr(PKT_CAR, 2); p.c = MakeCar(0x1234, 7);
        Inject(p);
        REQUIRE(Net::PeerCar() != nullptr);
        CHECK_EQ(Net::PeerCar()->carKey, 0x1234);
        CHECK_EQ(Net::PeerCar()->custom[100], 7);

        p.h.seq = 3; p.c = CarInfo{};                // "no car" (menus): nothing to build
        Inject(p);
        CHECK(Net::PeerCar() == nullptr);
        p.h.seq = 4; p.c = MakeCar(0x5678, 9);
        Inject(p);
    }
    {   // next launch: races load before the peer is driving, so the last car is read back
        NetFixture f(false);
        CHECK(Net::PeerCar() == nullptr);
        Net::SetDataDir(dir.c_str());
        REQUIRE(Net::PeerCar() != nullptr);
        CHECK_EQ(Net::PeerCar()->carKey, 0x5678);
        CHECK_EQ(Net::PeerCar()->custom[CUSTOM_RECORD_SIZE - 1], 9);
    }
    {   // a truncated file is ignored
        FILE* fp = nullptr;
        fopen_s(&fp, file.c_str(), "wb");
        REQUIRE(fp != nullptr);
        fwrite("abc", 1, 3, fp);
        fclose(fp);
        NetFixture f(false);
        Net::SetDataDir(dir.c_str());
        CHECK(Net::PeerCar() == nullptr);
    }
    DeleteFileA(file.c_str());
    RemoveDirectoryA(dir.c_str());
}
