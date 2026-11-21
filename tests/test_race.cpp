// Races in co-op (src/raceutil.h): finishing as the winner when the other player won, the place the
// race script gets for our racer, and which of our racers follows a host opponent.
#include "mwt_test.h"
#include "../src/raceutil.h"

using namespace CoopRace;

static FinishInputs Ready() {
    FinishInputs in{};
    in.sharedWins = true; in.peerWon = true; in.racing = true; in.haveLocalRacer = true;
    in.localFinished = false; in.timerRunning = true; in.restartPending = false; in.cutscene = false;
    in.raceType = 0; in.alreadyTried = false;
    return in;
}

TEST(race_finish_as_winner_when_partner_won) {
    CHECK(ShouldFinishAsWinner(Ready()));
    for (int t = 0; t <= 5; ++t) { FinishInputs in = Ready(); in.raceType = t; CHECK(ShouldFinishAsWinner(in)); }
}

TEST(race_finish_as_winner_needs_every_condition) {
    FinishInputs in;
    in = Ready(); in.sharedWins = false;     CHECK(!ShouldFinishAsWinner(in));
    in = Ready(); in.peerWon = false;        CHECK(!ShouldFinishAsWinner(in));   // they are 2nd, or still racing
    in = Ready(); in.racing = false;         CHECK(!ShouldFinishAsWinner(in));   // free roam / results
    in = Ready(); in.haveLocalRacer = false; CHECK(!ShouldFinishAsWinner(in));
    in = Ready(); in.localFinished = true;   CHECK(!ShouldFinishAsWinner(in));   // crossed the line ourselves
    in = Ready(); in.timerRunning = false;   CHECK(!ShouldFinishAsWinner(in));   // countdown: script not in "racing"
    in = Ready(); in.restartPending = true;  CHECK(!ShouldFinishAsWinner(in));
    in = Ready(); in.cutscene = true;        CHECK(!ShouldFinishAsWinner(in));
    in = Ready(); in.alreadyTried = true;    CHECK(!ShouldFinishAsWinner(in));   // once per win, never twice
}

TEST(race_finish_only_finish_line_events) {
    FinishInputs in = Ready();
    in.raceType = -1; CHECK(!ShouldFinishAsWinner(in));   // unknown
    in.raceType = 6;  CHECK(!ShouldFinishAsWinner(in));
    in.raceType = 8;  CHECK(!ShouldFinishAsWinner(in));   // pursuit challenge
    CHECK(FinishLineRaceType(4)); CHECK(!FinishLineRaceType(7));
}

TEST(race_placing_for_script) {
    CHECK_EQ(PlacingForScript(true, true, 2), 1);    // their racer crossed our line first: we still won
    CHECK_EQ(PlacingForScript(true, true, 1), 1);
    CHECK_EQ(PlacingForScript(true, false, 3), 3);   // they didn't win: the game's place
    CHECK_EQ(PlacingForScript(false, true, 2), 2);   // an AI racer keeps its place
    CHECK_EQ(PlacingForScript(false, true, 1), 1);
}

TEST(race_pick_racer_same_model_first) {
    RacerCand c[3] = { { 0x11, true }, { 0x22, true }, { 0x33, true } };
    CHECK_EQ(PickRacer(c, 3, 0x22, false), 1);
    CHECK_EQ(PickRacer(c, 3, 0x22, true), 1);         // same model wins even when any would do
    c[1].free = false;                                 // taken by another mirror / the partner's car
    CHECK_EQ(PickRacer(c, 3, 0x22, false), -1);       // waits for a moment...
    CHECK_EQ(PickRacer(c, 3, 0x22, true), 0);         // ... then takes any free racer
}

TEST(race_pick_racer_rival_of_another_model) {
    // rival race: our game built another car as the rival than the host did ("no racer of model")
    RacerCand c[1] = { { 0xAAAA0001u, true } };
    CHECK_EQ(PickRacer(c, 1, 0x79054B53u, false), -1);
    CHECK_EQ(PickRacer(c, 1, 0x79054B53u, true), 0);
    RacerCand none[1] = { { 0x79054B53u, false } };
    CHECK_EQ(PickRacer(none, 1, 0x79054B53u, true), -1);   // nothing free: nothing bound
    CHECK_EQ(PickRacer(none, 0, 0x79054B53u, true), -1);
    CHECK(ANY_MODEL_AFTER_MS > 0);
}
