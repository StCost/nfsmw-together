#pragma once
// Races in co-op: the decisions that don't need the game, so the test suite covers them
// (tests\test_race.cpp). coopwin.inl and traffic.inl read the game, ask these, and do what they say.
#include <cstdint>

namespace CoopRace {
    // GRaceParameters race type (0x5FAA20): 0 sprint, 1 circuit, 2 drag, 3 lap knockout, 4 tollbooth,
    // 5 speedtrap. These end at the finish line through the basic race script; the others (pursuit
    // challenges, ...) end some other way and are left alone.
    inline bool FinishLineRaceType(int type) { return type >= 0 && type <= 5; }

    // The other player crossed the line first: finish our racer as the winner right away, the way the
    // race script finishes a racer at the finish line (see coopwin.inl).
    struct FinishInputs {
        bool sharedWins;      // [Career] SharedWins
        bool peerWon;         // they finished 1st in this run of the event we are racing
        bool racing;          // GRaceStatus play mode 1 (racing)
        bool haveLocalRacer;  // our GRacerInfo exists
        bool localFinished;   // ... and already finished (by itself or by us)
        bool timerRunning;    // our race clock runs: the countdown is over (the race script is in "racing")
        bool restartPending;  // a restart is on its way
        bool cutscene;        // a cutscene plays over the race
        int  raceType;        // GRaceParameters type
        bool alreadyTried;    // we finished it once for this win (never twice)
    };
    inline bool ShouldFinishAsWinner(const FinishInputs& in) {
        return in.sharedWins && in.peerWon && in.racing && in.haveLocalRacer && !in.localFinished &&
               in.timerRunning && !in.restartPending && !in.cutscene && FinishLineRaceType(in.raceType) &&
               !in.alreadyTried;
    }

    // The race script asks CalculateRanking for a racer's place when it crosses the line and passes the
    // answer on (NotifyRacePlacement: 1 = the winning flow, else the losing one). Our own racer's place
    // is 1 once the other player has won this event; everyone else keeps the game's answer.
    inline int PlacingForScript(bool isLocalRacer, bool peerWon, int gamePlacing) {
        return (isLocalRacer && peerWon) ? 1 : gamePlacing;
    }

    // Racer mirrors (client): each opponent of the host drives one of our own AI racers. The same model
    // first; when none of that model is free for a while (our game built another car for that opponent,
    // e.g. a different rival), any free racer, so nobody races on their own AI next to the host's.
    constexpr double ANY_MODEL_AFTER_MS = 1500.0;
    struct RacerCand { uint32_t model; bool free; };
    inline int PickRacer(const RacerCand* c, int n, uint32_t model, bool anyModel) {
        for (int i = 0; i < n; ++i) if (c[i].free && c[i].model == model) return i;
        if (anyModel) for (int i = 0; i < n; ++i) if (c[i].free) return i;
        return -1;
    }

    // Rival races together: the helper's car for Blacklist rival #bin - a car the rival's race intro shows
    // besides the rival's own; null = none: the game's own pick. Seen by starting each rival's first race
    // and logging the cars its intro cutscene builds: all others build only traffic / police props
    // (traf*, copmidsize_nis, log trucks) or nothing.
    inline const char* BossHelperCar(int bin) {
        switch (bin) {
        case 6:  return "imprezawrx";   // Ming (6.1.1: imprezawrx, eclipsegt)
        case 10: return "eclipsegt";    // 10.2.1
        case 11: return "eclipsegt";    // Big Lou (11.1.2)
        default: return nullptr;
        }
    }

    // The other player's win counts for us only in a race we drove together: they were racing (not finished)
    // while our own race was running (clock on, not finished) for RacedTogetherMs. `since` keeps the start
    // of the current overlap (0 = none). True once it has lasted long enough.
    constexpr unsigned long RacedTogetherMs = 3000;
    inline bool RacedTogether(bool peerRacing, bool weRacing, unsigned long& since, unsigned long now) {
        if (!peerRacing || !weRacing) { since = 0; return false; }
        if (!since) { since = now ? now : 1; return false; }
        return now - since >= RacedTogetherMs;
    }
}
