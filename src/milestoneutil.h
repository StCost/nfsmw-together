#pragma once
// Shared milestones ([Career] SharedMilestones): the decisions that don't need the game, so the test
// suite covers them (tests\test_milestone.cpp). milestone.inl reads the game, asks these, and does what
// they say.
//
// The game keeps two lists in GManager (*0x91E00C), both of 20-byte entries:
//  milestones  (+0xA4 count, +0xA8 array): +0 type key, +4 milestone key, +8 state byte
//              (1 locked, 2 available, 3 reached - paid when the pursuit ends, 4 done), +9 flags,
//              +0xA bin (rival number), +0xC goal, +0x10 recorded value
//  speed traps (+0xAC count, +0xB0 array): +0 flags word (1 unlocked, 2 active, 4 done), +2 bin,
//              +4 trigger key, +0xC required value, +0x10 recorded speed
// A rival's milestones / speed traps are switched to available when their bin starts (0x62C330), so
// "available here" already means "the same rival here".
#include <cstdint>
#include <cmath>
#include <vector>

namespace SharedMilestones {
    enum Kind : uint8_t { KIND_MILESTONE = 0, KIND_SPEEDTRAP = 1, KIND_COUNT = 2 };
    enum : uint8_t  { MS_LOCKED = 1, MS_AVAILABLE = 2, MS_REACHED = 3, MS_DONE = 4 };
    enum : uint16_t { ST_UNLOCKED = 1, ST_ACTIVE = 2, ST_DONE = 4 };

    constexpr int MAX_ENTRIES = 1024;    // more than that in GManager = not the list we know
    constexpr int MAX_PER_TICK = 4;      // more completions at once = a career load / reset, not play
    constexpr uint16_t MAX_BIN = 64;
    constexpr float MAX_VALUE = 1e7f;

    inline bool MilestoneOpen(uint8_t state) { return state == MS_AVAILABLE || state == MS_REACHED; }
    inline bool MilestoneDone(uint8_t state) { return state == MS_DONE; }
    inline bool SpeedTrapOpen(uint16_t f) { return (f & (ST_UNLOCKED | ST_ACTIVE)) == (ST_UNLOCKED | ST_ACTIVE) && !(f & ST_DONE); }
    inline bool SpeedTrapDone(uint16_t f) { return (f & ST_DONE) != 0; }

    // One entry as read from the game (either list).
    struct Snap {
        uint8_t  kind = 0;
        uint32_t key = 0;       // milestone key / speed trap trigger key (same on every PC: game data)
        uint16_t bin = 0;
        bool     open = false;  // can still be completed here
        bool     done = false;
        float    value = 0;     // recorded value (milestone) / recorded speed (speed trap)
    };
    inline Snap FromMilestone(uint32_t key, uint8_t state, uint16_t bin, float recorded) {
        Snap s; s.kind = KIND_MILESTONE; s.key = key; s.bin = bin;
        s.open = MilestoneOpen(state); s.done = MilestoneDone(state); s.value = recorded;
        return s;
    }
    inline Snap FromSpeedTrap(uint32_t key, uint16_t flags, uint16_t bin, float recorded) {
        Snap s; s.kind = KIND_SPEEDTRAP; s.key = key; s.bin = bin;
        s.open = SpeedTrapOpen(flags); s.done = SpeedTrapDone(flags); s.value = recorded;
        return s;
    }

    // What travels: "I completed this one".
    struct Msg { uint8_t kind = 0; uint32_t key = 0; uint16_t bin = 0; float value = 0; };

    inline bool ValidMsg(uint8_t kind, uint32_t key, uint16_t bin, float value) {
        return kind < KIND_COUNT && key != 0 && bin <= MAX_BIN && std::isfinite(value) && std::fabs(value) <= MAX_VALUE;
    }

    // Finds completions between two reads of the lists. The arrays don't move or reorder while the world
    // is loaded, so entries are compared by position; a changed length or key means a different list
    // (career loaded / reset): that read becomes the new baseline and nothing counts as completed.
    // Only open -> done counts: a done entry that was never seen open is old progress.
    struct Tracker {
        std::vector<Snap> prev;
        bool have = false;
        void Reset() { prev.clear(); have = false; }
        // Returns how many entries were completed since the last call (written to out, up to maxOut),
        // or -1 when this read was taken as a new baseline (first read, different list, or more than
        // MAX_PER_TICK completions at once).
        int Update(const Snap* cur, int n, Msg* out, int maxOut) {
            if (n < 0 || n > 2 * MAX_ENTRIES) n = 0;
            bool same = have && (int)prev.size() == n;
            for (int i = 0; same && i < n; ++i) same = prev[i].kind == cur[i].kind && prev[i].key == cur[i].key;
            int found = 0;
            if (same) {
                for (int i = 0; i < n; ++i) if (prev[i].open && cur[i].done) ++found;
            }
            bool baseline = !same || found > MAX_PER_TICK;
            int written = 0;
            if (!baseline) {
                for (int i = 0; i < n && written < maxOut; ++i) {
                    if (!(prev[i].open && cur[i].done)) continue;
                    Msg& m = out[written++];
                    m.kind = cur[i].kind; m.key = cur[i].key; m.bin = cur[i].bin;
                    m.value = std::isfinite(cur[i].value) && std::fabs(cur[i].value) <= MAX_VALUE ? cur[i].value : 0.f;
                }
            }
            prev.assign(cur, cur + n);
            have = true;
            return baseline ? -1 : written;
        }
    };

    // Small set of (kind, key) remembered for a while: completions we applied from the other player
    // (not sent back), and messages already handled (the sender repeats each one).
    struct KeyRing {
        struct E { uint8_t kind; uint32_t key; };
        E e[64] = {};
        int n = 0, head = 0;
        bool Has(uint8_t kind, uint32_t key) const {
            for (int i = 0; i < n; ++i) if (e[i].kind == kind && e[i].key == key) return true;
            return false;
        }
        void Add(uint8_t kind, uint32_t key) {
            if (Has(kind, key)) return;
            e[head] = { kind, key };
            head = (head + 1) % 64;
            if (n < 64) ++n;
        }
        void Clear() { n = head = 0; }
    };

    // Repeats: each completion goes out at once and again 1.5 s and 4 s later (UDP may drop one).
    struct Outbox {
        struct Item { Msg m; uint32_t id; uint32_t nextAt; int left; };
        std::vector<Item> items;
        static constexpr int MAX_ITEMS = 32;
        void Push(const Msg& m, uint32_t id, uint32_t now) {
            if ((int)items.size() >= MAX_ITEMS) items.erase(items.begin());
            items.push_back({ m, id, now, 3 });
        }
        // Calls send(msg, id) for every item due now; returns how many were sent.
        template <class F> int Flush(uint32_t now, F send) {
            int sent = 0;
            for (size_t i = 0; i < items.size();) {
                Item& it = items[i];
                if ((int32_t)(now - it.nextAt) >= 0) {
                    send(it.m, it.id); ++sent;
                    --it.left;
                    it.nextAt = now + (it.left == 2 ? 1500u : 2500u);
                    if (it.left <= 0) { items.erase(items.begin() + i); continue; }
                }
                ++i;
            }
            return sent;
        }
        void Clear() { items.clear(); }
    };

    // Messages from the other player wait here until this game can take them (free roam, no cutscene).
    struct Inbox {
        struct Item { Msg m; uint32_t at; };
        std::vector<Item> items;
        static constexpr int MAX_ITEMS = 32;
        static constexpr uint32_t KEEP_MS = 10 * 60 * 1000;   // a message older than this is dropped
        KeyRing seen;                                        // handled already (repeats are ignored)
        // false: a repeat of one we have (queued or handled), or the queue is full
        bool Push(const Msg& m, uint32_t now) {
            if (seen.Has(m.kind, m.key)) return false;
            for (auto& it : items) if (it.m.kind == m.kind && it.m.key == m.key) return false;
            if ((int)items.size() >= MAX_ITEMS) return false;
            items.push_back({ m, now });
            return true;
        }
        void DropOld(uint32_t now) {
            for (size_t i = 0; i < items.size();)
                if (now - items[i].at > KEEP_MS) items.erase(items.begin() + i); else ++i;
        }
        void Done(size_t i) {
            if (i >= items.size()) return;
            seen.Add(items[i].m.kind, items[i].m.key);
            items.erase(items.begin() + i);
        }
        void Clear() { items.clear(); seen.Clear(); }
    };

    // What to do with one received completion, given our own entry (if any).
    enum Verdict { V_APPLY, V_NOT_FOUND, V_ALREADY_DONE, V_NOT_AVAILABLE, V_OTHER_RIVAL };
    inline Verdict Judge(bool found, const Snap& mine, uint16_t msgBin, uint8_t ourBin) {
        if (!found) return V_NOT_FOUND;
        if (mine.done) return V_ALREADY_DONE;
        if (!mine.open) return V_NOT_AVAILABLE;
        // same rival: the entry's bin, the sender's bin and our career's rival all agree (0 = unknown here)
        if (mine.bin != msgBin || (ourBin && mine.bin != ourBin)) return V_OTHER_RIVAL;
        return V_APPLY;
    }
    inline const char* VerdictText(Verdict v) {
        switch (v) {
        case V_APPLY: return "applied";
        case V_NOT_FOUND: return "not in this career";
        case V_ALREADY_DONE: return "already done here";
        case V_NOT_AVAILABLE: return "not available here (another rival)";
        case V_OTHER_RIVAL: return "another rival here";
        }
        return "?";
    }
}
