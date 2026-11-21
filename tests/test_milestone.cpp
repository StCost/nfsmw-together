// Shared milestones (src/milestoneutil.h): finding our completions, repeats, the receiver's queue and
// verdict, and the milestone packet through the real serializer and handler.
#include "net_helpers.h"
#include "../src/milestoneutil.h"
#include <limits>

using namespace SharedMilestones;

namespace {
    Snap Ms(uint32_t key, uint8_t state, uint16_t bin = 15, float value = 0) { return FromMilestone(key, state, bin, value); }
    Snap Trap(uint32_t key, uint16_t flags, uint16_t bin = 15, float value = 0) { return FromSpeedTrap(key, flags, bin, value); }
}

// ---- entry states ----------------------------------------------------------------------------------
TEST(milestone_states) {
    CHECK(!MilestoneOpen(MS_LOCKED)); CHECK(MilestoneOpen(MS_AVAILABLE)); CHECK(MilestoneOpen(MS_REACHED));
    CHECK(!MilestoneOpen(MS_DONE)); CHECK(MilestoneDone(MS_DONE)); CHECK(!MilestoneDone(MS_REACHED));
    CHECK(!MilestoneOpen(0)); CHECK(!MilestoneDone(0));
    // speed traps: open only when unlocked AND active (the current rival's), not done
    CHECK(SpeedTrapOpen(ST_UNLOCKED | ST_ACTIVE));
    CHECK(!SpeedTrapOpen(ST_UNLOCKED));
    CHECK(!SpeedTrapOpen(ST_ACTIVE));
    CHECK(!SpeedTrapOpen(ST_UNLOCKED | ST_ACTIVE | ST_DONE));
    CHECK(SpeedTrapDone(ST_UNLOCKED | ST_DONE));
    Snap s = Ms(0x1234, MS_REACHED, 14, 2.5f);
    CHECK_EQ(s.kind, KIND_MILESTONE); CHECK_EQ(s.key, 0x1234u); CHECK_EQ(s.bin, 14); CHECK(s.open); CHECK(!s.done);
    Snap t = Trap(0x55, ST_UNLOCKED | ST_DONE, 13, 61.f);
    CHECK_EQ(t.kind, KIND_SPEEDTRAP); CHECK(!t.open); CHECK(t.done); CHECK_NEAR(t.value, 61.f, 1e-6);
}

// ---- finding completions ---------------------------------------------------------------------------
TEST(milestone_tracker_first_read_is_a_baseline) {
    Tracker tr; Msg out[8];
    Snap a[3] = { Ms(1, MS_DONE), Ms(2, MS_AVAILABLE), Trap(3, ST_UNLOCKED | ST_DONE) };
    CHECK_EQ(tr.Update(a, 3, out, 8), -1);         // old progress is never sent
    CHECK_EQ(tr.Update(a, 3, out, 8), 0);          // nothing changed
}

TEST(milestone_tracker_open_to_done_is_reported) {
    Tracker tr; Msg out[8];
    Snap a[4] = { Ms(1, MS_AVAILABLE, 15), Ms(2, MS_LOCKED, 14), Trap(3, ST_UNLOCKED | ST_ACTIVE, 15), Trap(4, ST_UNLOCKED, 14) };
    tr.Update(a, 4, out, 8);
    a[0] = Ms(1, MS_REACHED, 15);                    // reached during a pursuit: not done yet
    CHECK_EQ(tr.Update(a, 4, out, 8), 0);
    a[0] = Ms(1, MS_DONE, 15, 125.f);                // paid when the pursuit ended
    a[2] = Trap(3, ST_UNLOCKED | ST_DONE, 15, 70.f); // speed trap passed
    int n = tr.Update(a, 4, out, 8);
    REQUIRE(n == 2);
    CHECK_EQ(out[0].kind, KIND_MILESTONE); CHECK_EQ(out[0].key, 1u); CHECK_EQ(out[0].bin, 15); CHECK_NEAR(out[0].value, 125.f, 1e-6);
    CHECK_EQ(out[1].kind, KIND_SPEEDTRAP); CHECK_EQ(out[1].key, 3u); CHECK_NEAR(out[1].value, 70.f, 1e-6);
    CHECK_EQ(tr.Update(a, 4, out, 8), 0);           // reported once
}

TEST(milestone_tracker_locked_to_done_is_not_play) {
    Tracker tr; Msg out[8];
    Snap a[1] = { Ms(9, MS_LOCKED) };
    tr.Update(a, 1, out, 8);
    a[0] = Ms(9, MS_DONE);                           // never open here: a save loaded, not a completion
    CHECK_EQ(tr.Update(a, 1, out, 8), 0);
}

TEST(milestone_tracker_new_list_is_a_baseline) {
    Tracker tr; Msg out[8];
    Snap a[2] = { Ms(1, MS_AVAILABLE), Ms(2, MS_AVAILABLE) };
    tr.Update(a, 2, out, 8);
    Snap b[3] = { Ms(1, MS_DONE), Ms(2, MS_DONE), Ms(5, MS_AVAILABLE) };   // other length
    CHECK_EQ(tr.Update(b, 3, out, 8), -1);
    Snap c[3] = { Ms(1, MS_DONE), Ms(7, MS_DONE), Ms(5, MS_AVAILABLE) };   // other key at the same place
    CHECK_EQ(tr.Update(c, 3, out, 8), -1);
    tr.Reset();
    CHECK_EQ(tr.Update(c, 3, out, 8), -1);
}

TEST(milestone_tracker_mass_change_is_a_reload) {
    Tracker tr; Msg out[16];
    Snap a[MAX_PER_TICK + 1];
    for (int i = 0; i < MAX_PER_TICK + 1; ++i) a[i] = Ms((uint32_t)(i + 1), MS_AVAILABLE);
    tr.Update(a, MAX_PER_TICK + 1, out, 16);
    for (auto& s : a) s = Ms(s.key, MS_DONE);
    CHECK_EQ(tr.Update(a, MAX_PER_TICK + 1, out, 16), -1);    // a career reset, not play
    // up to MAX_PER_TICK at once is still play (a pursuit ending pays several)
    Tracker t2;
    for (int i = 0; i < MAX_PER_TICK; ++i) a[i] = Ms((uint32_t)(i + 1), MS_REACHED);
    t2.Update(a, MAX_PER_TICK, out, 16);
    for (int i = 0; i < MAX_PER_TICK; ++i) a[i] = Ms((uint32_t)(i + 1), MS_DONE);
    CHECK_EQ(t2.Update(a, MAX_PER_TICK, out, 16), MAX_PER_TICK);
}

TEST(milestone_tracker_limits_output_and_odd_values) {
    Tracker tr; Msg out[1];
    Snap a[2] = { Ms(1, MS_AVAILABLE), Ms(2, MS_AVAILABLE) };
    tr.Update(a, 2, out, 1);
    a[0] = Ms(1, MS_DONE, 15, std::numeric_limits<float>::quiet_NaN());
    a[1] = Ms(2, MS_DONE);
    CHECK_EQ(tr.Update(a, 2, out, 1), 1);           // only as many as fit
    CHECK_EQ(out[0].value, 0);                       // a NaN value never travels
    CHECK_EQ(tr.Update(nullptr, -5, out, 1), -1);   // nonsense length: empty list, baseline
}

// ---- repeats -------------------------------------------------------------------------------------------
TEST(milestone_outbox_repeats_three_times) {
    Outbox ob; std::vector<uint32_t> sentAt;
    Msg m; m.kind = KIND_MILESTONE; m.key = 42; m.bin = 15;
    ob.Push(m, 7, 1000);
    auto send = [&](const Msg& x, uint32_t id) { CHECK_EQ(x.key, 42u); CHECK_EQ(id, 7u); };
    CHECK_EQ(ob.Flush(1000, send), 1);              // at once
    CHECK_EQ(ob.Flush(2000, send), 0);
    CHECK_EQ(ob.Flush(2500, send), 1);              // 1.5 s later
    CHECK_EQ(ob.Flush(4000, send), 0);
    CHECK_EQ(ob.Flush(5000, send), 1);              // 2.5 s after that
    CHECK(ob.items.empty());
    CHECK_EQ(ob.Flush(99999, send), 0);
}

TEST(milestone_outbox_is_bounded) {
    Outbox ob; Msg m;
    for (int i = 0; i < Outbox::MAX_ITEMS + 10; ++i) { m.key = (uint32_t)(i + 1); ob.Push(m, (uint32_t)i, 0); }
    CHECK_EQ(ob.items.size(), Outbox::MAX_ITEMS);
    CHECK_EQ(ob.items[0].m.key, 11u);               // the oldest went
}

// ---- receiving -----------------------------------------------------------------------------------------
TEST(milestone_inbox_dedupes_and_expires) {
    Inbox in; Msg m; m.kind = KIND_MILESTONE; m.key = 5; m.bin = 15;
    CHECK(in.Push(m, 0));
    CHECK(!in.Push(m, 10));                          // a repeat while queued
    Msg t = m; t.kind = KIND_SPEEDTRAP;               // same key, other list: a different entry
    CHECK(in.Push(t, 10));
    CHECK_EQ(in.items.size(), 2u);
    in.Done(0);
    CHECK(!in.Push(m, 20));                          // a repeat after it was handled
    CHECK_EQ(in.items.size(), 1u);
    in.DropOld(Inbox::KEEP_MS + 11);
    CHECK(in.items.empty());
    in.Done(5);                                       // out of range: nothing happens
    Msg x; for (int i = 0; i < Inbox::MAX_ITEMS + 3; ++i) { x.key = (uint32_t)(100 + i); in.Push(x, 0); }
    CHECK_EQ(in.items.size(), Inbox::MAX_ITEMS);
}

TEST(milestone_keyring_forgets_the_oldest) {
    KeyRing r;
    for (uint32_t k = 1; k <= 64; ++k) r.Add(0, k);
    CHECK(r.Has(0, 1)); CHECK(r.Has(0, 64)); CHECK(!r.Has(1, 1));
    r.Add(0, 65);
    CHECK(!r.Has(0, 1)); CHECK(r.Has(0, 65)); CHECK(r.Has(0, 2));
    r.Add(0, 65);                                     // already there: no slot used
    CHECK(r.Has(0, 2));
    r.Clear(); CHECK(!r.Has(0, 65));
}

TEST(milestone_judge_same_rival_only) {
    CHECK_EQ(Judge(false, Snap{}, 15, 15), V_NOT_FOUND);
    CHECK_EQ(Judge(true, Ms(1, MS_DONE, 15), 15, 15), V_ALREADY_DONE);
    CHECK_EQ(Judge(true, Ms(1, MS_LOCKED, 14), 14, 15), V_NOT_AVAILABLE);    // their rival, not ours
    CHECK_EQ(Judge(true, Ms(1, MS_AVAILABLE, 15), 15, 15), V_APPLY);
    CHECK_EQ(Judge(true, Ms(1, MS_REACHED, 15), 15, 15), V_APPLY);           // reached here too: pay it now
    CHECK_EQ(Judge(true, Ms(1, MS_AVAILABLE, 15), 14, 15), V_OTHER_RIVAL);   // the sender meant another rival
    CHECK_EQ(Judge(true, Ms(1, MS_AVAILABLE, 15), 15, 14), V_OTHER_RIVAL);   // left over from a rival we beat
    CHECK_EQ(Judge(true, Ms(1, MS_AVAILABLE, 15), 15, 0), V_APPLY);          // our rival unknown: the state decides
    CHECK_EQ(Judge(true, Trap(1, ST_UNLOCKED | ST_ACTIVE, 15), 15, 15), V_APPLY);
    CHECK_EQ(Judge(true, Trap(1, ST_UNLOCKED, 15), 15, 15), V_NOT_AVAILABLE);
    CHECK_STR(VerdictText(V_APPLY), "applied");
    CHECK_STR(VerdictText((Verdict)99), "?");
}

TEST(milestone_valid_msg) {
    CHECK(ValidMsg(0, 1, 15, 0.f));
    CHECK(ValidMsg(1, 1, 64, 80.f));
    CHECK(!ValidMsg(2, 1, 15, 0.f));
    CHECK(!ValidMsg(0, 0, 15, 0.f));
    CHECK(!ValidMsg(0, 1, 65, 0.f));
    CHECK(!ValidMsg(0, 1, 15, std::numeric_limits<float>::infinity()));
    CHECK(!ValidMsg(0, 1, 15, 2e7f));
}

// ---- the packet ----------------------------------------------------------------------------------------
namespace {
    struct MsSeen { int calls = 0; uint32_t id = 0; uint8_t kind = 0; uint32_t key = 0; uint16_t bin = 0; float value = 0; };
    MsSeen g_ms;
    void HookMilestone() {
        g_ms = MsSeen{};
        Net::OnMilestone = [](uint32_t id, uint8_t kind, uint32_t key, uint16_t bin, float value) {
            ++g_ms.calls; g_ms.id = id; g_ms.kind = kind; g_ms.key = key; g_ms.bin = bin; g_ms.value = value;
        };
    }
    std::vector<std::vector<uint8_t>> SendOnHost(uint32_t id, uint8_t kind, uint32_t key, uint16_t bin, float value) {
        th::NetFixture host(true);
        th::Connect("Client");
        th::g_sent.clear();
        Net::SendMilestone(id, kind, key, bin, value);
        return th::g_sent;
    }
}

TEST(milestone_packet_round_trip) {
    auto d = SendOnHost(77, KIND_SPEEDTRAP, 0xCAFEF00D, 12, 63.5f);
    REQUIRE(d.size() == 2);                           // sent twice
    CHECK_EQ(d[0].size(), sizeof(MilestonePacket));
    th::NetFixture client(false);
    th::Connect("Host");
    HookMilestone();
    for (auto& p : d) th::InjectRaw(p.data(), (int)p.size());
    CHECK_EQ(g_ms.calls, 1);                          // the copy is dropped
    CHECK_EQ(g_ms.id, 77u); CHECK_EQ(g_ms.kind, KIND_SPEEDTRAP); CHECK_EQ(g_ms.key, 0xCAFEF00Du);
    CHECK_EQ(g_ms.bin, 12); CHECK_NEAR(g_ms.value, 63.5f, 1e-6);
}

TEST(milestone_packet_rejects_bad_input) {
    th::NetFixture client(false);
    th::Connect("Host");
    HookMilestone();
    MilestonePacket p{};
    p.h = th::Hdr(PKT_MILESTONE, 5);
    p.msgId = 1; p.key = 9; p.bin = 15; p.kind = 0; p.value = 1.f;
    th::InjectRaw(&p, (int)sizeof(p) - 1);           // short
    CHECK_EQ(g_ms.calls, 0);
    MilestonePacket q = p; q.kind = 2; q.msgId = 2;  th::InjectRaw(&q, sizeof(q));
    q = p; q.key = 0; q.msgId = 3;                    th::InjectRaw(&q, sizeof(q));
    q = p; q.bin = 200; q.msgId = 4;                  th::InjectRaw(&q, sizeof(q));
    q = p; q.value = std::numeric_limits<float>::quiet_NaN(); q.msgId = 5; th::InjectRaw(&q, sizeof(q));
    q = p; q.value = 1e30f; q.msgId = 6;              th::InjectRaw(&q, sizeof(q));
    CHECK_EQ(g_ms.calls, 0);
    th::InjectRaw(&p, sizeof(p));
    CHECK_EQ(g_ms.calls, 1);
    th::InjectRaw(&p, sizeof(p));                     // same id again
    CHECK_EQ(g_ms.calls, 1);
    p.msgId = 8; th::InjectRaw(&p, sizeof(p));        // a new id: the game layer dedupes by key
    CHECK_EQ(g_ms.calls, 2);
}

TEST(milestone_not_sent_while_disconnected) {
    th::NetFixture host(true);
    th::g_sent.clear();
    Net::SendMilestone(1, 0, 5, 15, 0.f);
    CHECK(th::g_sent.empty());
}
