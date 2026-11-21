# Testing NFS MW Together

There are three layers. Use all three before a release, and the first one before every commit.

| Layer | What it covers | Needs |
|---|---|---|
| 1. Automated tests | network code, packets, config, save transfer, maths | nothing but the compiler |
| 2. One PC | start-up, solo racing, test tools, performance | the game |
| 3. Two PCs | everything co-op | the game on two PCs on the same network |

## 1. Automated tests (no game)

```
tests\build_tests.bat            build and run (also an AddressSanitizer pass if your MSVC has it)
tests\build_tests.bat noasan     faster
```

Exit code 0 and `N test(s) run, 0 failed` is a pass. Protocol changes must update `tests\test_protocol.cpp`
(struct sizes, offsets, `COOP_VERSION`) in the same commit: the tests fail on purpose until you do.

## 2. Test tools inside the mod

Set `[Debug] TestKeys=1` in `scripts\MWTogether.ini`. Nothing below works without it.

**Keys** (only while the game window is focused): `F5` autopilot on/off, `F7` start the test event
(`[Debug] TestEvent`), `F10`/`F11` spawn/remove a copy of the other player's car, `F12` spawn a cop.

**Command file.** Write one command per line into `scripts\MWTogether.cmd`; the mod reads it within half a
second, deletes it and logs `Command '<text>': <result>`. This works with the game in the background.

| Command | What it does |
|---|---|
| `event <id>` | start a career event for both players, e.g. `event 15.1.1` |
| `events` | list every event in the log: id, type, `BOSS`, preset car |
| `restart` | restart the current race |
| `slowai` | AI opponents stop driving (so a player wins) |
| `cash` | log career cash and whether the current event counts as won |
| `autopilot` | toggle the autopilot |
| `skipnis` | press "skip" on the current cutscene |
| `story` / `story <movie>` | log the story-scene state (movie, scene hash, the other player's scene, hold, rival number) / play a story movie over the world here as the career would, e.g. `story storyfmv_pin11`, `story blacklist_15` |
| `pursuit` / `uncop` | start a pursuit on you / remove all police cars |
| `ram` | first call: park 25 m from the other car. Second call (5 s later): hit it at 90 km/h |
| `ram cop` | the same against the nearest of the other player's police cars shown here |
| `pos` | log own position, speed and damage |
| `tp <x> <y> <z>` | move your car there (numbers as `pos` prints them). The first call loads the world at that spot; send the same `tp` again about 5 s later to move. Handy to reach a marker the autopilot won't drive to |
| `clone` / `unclone` / `cop` | spawn/remove a copy of the other player's car, spawn a cop |
| `notice <text>` | show a notice in the game's banner |
| `milestones` | log our rival's open and done milestones / speed traps (key, rival, value) and print the counts |
| `milestone` / `speedtrap` | complete the first open milestone / speed trap of our rival here, through the game's own award (bounty included), as if we had earned it. With co-op on it's then sent to the other player |
| `flash` | flash our headlights for 5 s, as if the speedbreaker bind were held (co-op on only) |
| `shot <file.bmp>` | save the next frame (without the overlay) |
| `accept <Screen.fng>` | send "accept" to a menu screen |

**Useful ini switches:** `[Debug] AutoPilot=1` (the AI drives in free roam and races), `[Window]
StartInBackground=1` (start without taking focus, muted), `[Debug] Perf=1` (fps and the mod's cost every
5 s), `[Debug] ReadyDelayMs=6000` (pretend to load slowly), `[Debug] BootTrace=1` (log the boot screens).

**Useful event ids:** `15.1.1` circuit, `15.1.2` rival race (Razor), `14.4.1` tollbooth, `13.5.1` speedtrap,
`12.7.3` drag, `15.3.1` lap knockout, `19.8.32` Challenge Series tollbooth (preset car). `events` lists all 208.

**The log** is `scripts\MWTogether.log`, rewritten on every start. The game keeps it open, so read it with
shared access (plain `Get-Content` works; some tools need "open for read, allow read/write").

## 3. Scenarios (`tools\coop-test.ps1`)

Run on the **host**. On the other PC just start the game and let it connect; for scenarios that restart
the host, restart the client too when it sees the host leave. Both PCs: same build, `TestKeys=1`,
`AutoPilot=1` unless the scenario says "parked".

```
tools\coop-test.ps1 -Scenario list
tools\coop-test.ps1 -Scenario race
tools\coop-test.ps1 -Scenario boss -Event 15.1.2
tools\coop-test.ps1 -Scenario solo -Solo
```

| Scenario | Pass looks like (H = host log, C = client log) |
|---|---|
| `smoke` | H+C `Connected to peer`, `Ghost acquired: spawned peer car`, a `joined` notice. No `CRASH`. |
| `race` | H+C `standings use the other player's own progress hooked` at start-up. C `Event: using the peer's opponents`, `our racer ... follows the host's opponent`, `the other player's grid slot x y z`, then on the grid `Start: we stood on the host's grid slot - moved to the slot of the host's racer (N m)` (N = a few metres); from then on neither log shows `spawn protection` freezing the car and both cars drive away at the go. The other player's name is in the standings during the intro and countdown. While racing `Race: peer pct A (sent B) ...`: A is a little above B at speed, and the rank flips within a car length when one passes the other (drive side by side, then let the other pass). After the finish C `the other player finished (rank 1 there, 1 here)`, `the other player won - counted as our win too`; C's `cash` goes up; C returns to free roam and its save file is rewritten. |
| `boss` | H `sent 1 opponent(s)`; C `opponent 1 built as the peer's opponent`, `Ghost acquired: spawned peer car` (the partner is not a racer here). Standings on each PC: that player + the rival. |
| `restart` | H `restarted here - telling the peer`, C `peer restarted the race - restarting here too`; C is back on the grid with a new countdown, logs `Start: we stood on the host's grid slot ...` again and the two cars stand apart. |
| `cutscene` | One vote: H `voted to skip ... waiting`, intro runs its full ~7.7 s on both. Both vote (`skipnis` on C too): `both voted - skipping`, intro ~3 s. |
| `pursuit` | The PC that is NOT chased logs `Cops: showing the other player's police car` and `Pursuit: mine no ... theirs heat 1.x, 1 of their cops shown here`. A parked car is busted in ~30 s: with `SharedPursuits=1` (default) the other player is busted right after (see Shared pursuits below); with 0 that player lands in the garage and the other keeps playing. |
| `ram` (client parked, `AutoPilot=0`) | H `hit the other player, lost ~60 km/h - sent to them`, C `hit by the other player, pushed by ~35 km/h`, C's `pos` moves ~4 m. No hit line the other way round. |
| `together` (client has `ReadyDelayMs=6000`) | H `on the grid first - waiting for <name> to load...` then `go (both ready, waited 6.x s)`. |
| `results` | C stays on its results screen after 15.1.1; H leaves its own and starts 15.1.2. C logs `invited while we're on the results screen - leaving it to join`, then `starting 15.1.2`; both race 15.1.2 together. |
| `kinds` | All four events run 70 s with no `CRASH` on either PC. C logs `Traffic: in a race - mirroring the host's traffic with our own traffic cars` (with `SharedInRaces=0`: `in a race - keeping our own traffic`), then `mirroring the host's traffic (N cars, M of the host's without a car here)` with N > 0 in tollbooth / drag / knockout; after the `in a race` line C logs no `KillVehicle` until `race over` (only mirrors of the host's cops may go). Drag (12.7.3) used to crash at ~28 s when the client removed its own traffic. |
| `solo` | Race alone: no `Start: on the grid` hold, no `Collision` lines, the win pays out as in the unmodded game. |

## Checks without a script

| Check | How | Pass |
|---|---|---|
| Join from far away | one player drives > 500 m away, the other starts an event | the joining PC logs `keeping our copy of the other player's car until their racer is built`, then `AI racer built as peer car` (or, if the model still isn't loaded, `their racer keeps this AI car's model`) and `Ghost acquired: racer built as peer car`; on both PCs the other player is in the standings with their name from the countdown on |
| Save backup | client plays until an autosave | H `Career: stored <name>'s save`, file in `scripts\MWTogether-saves\<name>\` |
| Save restore | client: close the game, move its save away, start again | C `restored save '<profile>' from the host (we had none)` before `pressing START`; the career loads |
| Background start | `StartInBackground=1`, keep another window focused | free roam reached, `Window: in the world - muted until clicked` |
| Launcher | run it, press PLAY | game starts; the ini keeps its comments |
| Smoothness | one car parked, the other on autopilot | `Ghost stats ... jitter` mostly under 20 mm |
| Performance | `Perf=1` | `mod` under about 0.2 ms/frame |
| Visual state | watch the partner's car | brake lights when it stops; `Ghost: damage zones now X` after it crashes |
| Join / leave | start and quit one game | banner `<name> joined` / `left` on the other |
| Shared race traffic | both PCs `AutoPilot=1`, start 14.4.1 / 12.7.3 / 15.3.1 with `event`, cars close together | C `our traffic car ... now follows the host's car N (same model ...)`; traffic around you is in the same places and models on both screens (a few extra cars on C are its spare own traffic, they keep their own AI); hitting a traffic car on C logs `Collision: hit the other player's car` and pushes it on H. After the finish C `race over`, `released N of our traffic cars`, and 8 s later its own traffic is replaced by mirrors as in free roam |
| Smooth traffic handover | `SmoothHandover=1` (default). Free roam, H parked in a busy street, C drives with traffic around it. (1) C drives straight away from H past 450 m and watches the cars around it. (2) C turns back and drives towards H, looking ahead | (1) C `Traffic: away from the host (... m) - using own traffic (the host's cars around us drive on as ours)`, then `the host's car N (...) drives on as our own traffic car ... (traffic AI, the game culls it)` and `left the host's traffic - N of the host's cars drive on as our traffic (M taken over by the game's traffic manager)`; no car around C disappears, they keep driving in their lanes (not stopped dead, not off the road); later `KillVehicle` lines only for cars out of sight. (2) C `near the host ... (our cars stay until out of view, host cars appear out of view)`, then every ~5 s `handover - X of our cars still in view, Y host cars waiting for a spot out of view, Z shown by our own cars ...` with X falling to 0; `our traffic car ... now shows the host's car N (... in its spot / nearby, in view ...)` when one of C's cars takes a host car's place (it glides there over ~1.5 s). No car appears or vanishes in front of C; no two cars inside each other. Rerun with `SmoothHandover=0`: the old instant switch (cars vanish / pop in) |
| No cars popping up in front | `NoSpawnNearPlayers=1` (default) on both. Start-up: both `Traffic: new traffic / cops never right in front of the other player - spot checks hooked: traffic ok, cops 4/4`. (1) Free roam, both drive side by side through busy streets, then 100-300 m apart (each with its own traffic / patrols, the client in a race with `SharedInRaces=0`). (2) A pursuit on H with C driving ahead of H. (3) C drives towards H's traffic looking ahead (near the 350 m switch) | (1)(2) now and then `Traffic: a new traffic car / police car at x y z would appear N m in front of the other player - the game picks another spot` (first few), and every ~5 s on change `Traffic: no-spawn - T traffic / P police spots refused in front of the other player so far, W of their cars wait until out of our view`; no new car or cop appears within ~60 m in front of either player; traffic density and pursuits otherwise look normal (cops keep coming, from other places). (3) C's `W` goes up while a host car's spot is in view and back to 0 once it is behind or far; no car pops up in front of C. No `CRASH`. Rerun with `NoSpawnNearPlayers=0`: no such lines |
| Rival catch-up | rival race (`boss -Event 15.1.2`), one player drives well ahead, the other slowly | the rival's PC (H) logs every 15 s `Race: catch-up measured against A% (ours B%, partner C%, rival race)` with A = the larger of B and C; the rival stays near the leading player on both screens, it never drops back to the slower one. If H ever logs `catch-up measured against us only (...)` while both race, the values in it say why the partner didn't count |

## Shared pursuits (`SharedPursuits`, `NoPursuitCams`)

Both PCs in free roam, linked, within ~300 m of each other, `TestKeys=1`, `AutoPilot=0` (parked unless a step
says to drive), `SharedPursuits=1` and `NoPursuitCams=1` on both. At start-up both logs show
`Pursuit: start camera in co-op off (NoPursuitCams=1)` and `Pursuit: shared pursuits on (minimap cops hooked, radar
detector hooked)`. While either player is chased, both logs print a status line every 5 s:
`Pursuit: shared - here <state> [seen], <s> s left, heat <h> | there <state> [seen], <s> s left, heat <h>`.

| Check | How | Pass |
|---|---|---|
| Heat | H: `pursuit` | H `Pursuit: ours none -> chase`. Within a second C logs `Pursuit: heat a -> b (the other player's heat)` with b = H's heat; as H's heat climbs, C follows. C's heat never goes down because of H. Same the other way round. |
| No start camera | H: `pursuit`, drive away from a patrol so a cop starts the chase by itself | H's camera stays behind the car; H logs `Pursuit: start camera skipped (co-op, NoPursuitCams=1)`. Alone (C not connected) the camera flies to the cop as in the unmodded game. |
| Radar / minimap | H: `cop` (a patrol cop 25 m from H), C drives within 300 m of it | C shows the cop (`Cops: showing the other player's police car`), C logs `Radar: 1 of the other player's cops in the radar detector` and the radar detector beeps. H: `pursuit`: C logs `Minimap: N of the other player's cops shown` and the cops blink red / blue on C's minimap, also while C is not chased. |
| Ram their cop | H: `cop`. C: `ram cop`, 5 s later `ram cop` again | C `Collision: hit the other player's car <id>`, then `Pursuit: hit the other player's cop <id> - starting a pursuit on us` and `Pursuit: ours none -> chase`. (H's own game may start a chase on H too: its cop was hit) |
| Busted together | both: `pursuit`; keep both parked | the first one busted (say H) logs `Pursuit: we were busted - the other player gets busted too (count 1)`; C logs `Pursuit: the other player was busted - busting us too` and `Pursuit: busted here too, as the other player was`; both get the game's busted screen. C not chased at that moment: `no pursuit here, starting one to bust us too`, then the same. |
| Cooldown waits | both: `pursuit`. H: `uncop` (H's cops are gone, H is out of sight), C stays chased | H `Pursuit: the other player is still seen - our cooldown waits (out of sight here)`; H's status line stays `chase` (no `cooldown`) for as long as C is seen, the cooldown bar doesn't run on H. |
| Escape together | as above, then C: `uncop` | both `Pursuit: both out of sight - cooldown runs as usual` (or `ours waits for theirs`), both `Pursuit: ours chase -> cooldown` within ~1 s, then both `Pursuit: ours cooldown -> escaped` within ~1 s of each other. If a new cop spots C during the cooldown, both go back: H logs `Pursuit: the other player is still seen` and H's state returns to `chase`. |
| Off | `SharedPursuits=0` on both | `Pursuit: SharedPursuits=0 - each player has their own heat and pursuit`; none of the lines above; busting or escaping affects only the player chased. |
| In events | start an event with `event 15.1.1` while chased | no heat / bust / cooldown lines while either player is in the event. |
## Notices and game controllers

One PC is enough. Keep the log open (`scripts\MWTogether.log`).

| Check | How | Pass |
|---|---|---|
| Notice length | free roam, `notice hello` in `MWTogether.cmd` (or a join / leave) | the banner slides in once, stays about `NoticeSeconds` (10 s), then slides out; no second slide-in. Log `Notice: holding the game banner for 10 s`. With `MenuSpeed=1` and `=2` the same length |
| Notice fallback | as above | no `banner timer object not found` / `exception while holding` lines. If one shows up, the banner has its old length (about 3 s) and the corner line still stays 10 s |
| Song banner | wait for a new song in free roam | the game's own song banner looks and times as before (we only hold our own notices) |
| Pad plugged in | start the game with no pad, get to free roam, plug a USB pad in | within about 2 s: `Pad: controller connected: <name> -> added to the game`; the pad drives the car. In the controller options the pad's bindings are shown |
| Bluetooth pad | as above, switch a paired Bluetooth pad on while playing | same as USB |
| Unplug + replug | pad connected at start, pull it out, wait 3 s, plug it back | `Pad: controller 0 disconnected (<name>) - removed from the game's list`, then `controller connected ... added`; the pad works again with the same bindings |
| Wheel force feedback | a force feedback wheel plugged in while playing | `Pad: force feedback slot 0: <name> (format 00000000, coop 00000000)`; the wheel's centering force works in a race |
| Bindings kept, no pad at start | change a pad binding in Options > Controller, save, quit. Start the game WITHOUT the pad, play until an autosave, quit. Start with the pad | log at the pad-less start `Pad: no controller attached - keeping the profile's controller bindings (made for XXXXXXXX)`; at the last start your changed binding is still there, no `reset to this pad's defaults` line |
| Bindings kept, pad added later | as above, but plug the pad in during the pad-less session | `Pad: keeping the controller bindings (made for X, this pad X)`; your bindings work at once |
| Restore defaults still works | Options > Controller > restore defaults | the bindings go back to the defaults, log `controller bindings set to the defaults ... (called from 0051D707)` |
| Unknown reset | play normally (pause menu, options, reset car, restart a race) | if `Pad: blocked a reset of the controller bindings (called from ...)` shows up, note what you just did: that is a reset the game would have done and the mod stopped |
| Switches off | `GamepadHotplug=0` | no `Pad: controller ...` lines, a pad plugged in later is ignored (the game's own behaviour) |

## Shared milestones (`[Career] SharedMilestones`)

Both PCs in free roam, linked, `TestKeys=1`, both careers at the **same** Blacklist rival (the overlay / `story`
shows the rival number; `milestones` prints it). At start-up both logs show
`Milestones: shared with the other player (same rival only)`.

| Check | How | Pass |
|---|---|---|
| Lists readable | both: `milestones` | `Command 'milestones': rival N: milestones M (a open, b done), speed traps T (c open, d done)` with the same N on both, plus one `Milestones: milestone XXXXXXXX rival N open/done ...` line per entry of that rival |
| Milestone shared | H: `milestone` | H `Command 'milestone': milestone K (rival N): applied, state now 4`, then `Milestones: we completed milestone K (rival N, ...) - telling the other player`; H's game shows the "milestone reached" popup. C: `Milestones: the other player completed milestone K`, then `Milestones: the other player's milestone K (rival N) completed here (state now 4)`; banner `<H> completed a milestone - it counts for you too`; C's bounty goes up by the milestone's reward; C's Blacklist / rap sheet shows K done. C sends nothing back. `milestones` on C: one more done. |
| Speed trap shared | C: `speedtrap` | the same lines the other way round with `speed trap`; on H the trap is done in the Blacklist screen |
| Real play | both: drive through one of the rival's speed traps (C only), or get a pursuit milestone (pursuit length, cost to state...) on H and escape | the same log lines as above within ~1 s of the game paying it on the PC that earned it (a pursuit milestone is paid when the pursuit ends, not when it's reached) |
| Already done | repeat a check for an entry the other PC already has | `... - already done here (our rival N)`, no notice, no second bounty |
| Different rivals | one career a rival further on | `... - not available here (another rival)` or `another rival here`; nothing changes on that PC |
| In a race | C starts `event 15.1.1`, then H: `milestone` | C logs `the other player completed ...` at once and applies it only after the race, back in free roam |
| Loading | quit to the menu and load the career again | `Milestones: the lists changed (career loaded?) ...` at most; no `we completed` lines for old progress |
| Off | `SharedMilestones=0` on both | `Milestones: SharedMilestones=0 - each player keeps their own`; no `Milestones:` sharing lines |

## Headlight flash (`[Coop] FlashLights`)

Both PCs in free roam, linked, near each other, `NoSpeedbreaker=1`. At start-up both logs show
`Lights: hold the speedbreaker bind to flash the headlights in co-op (render hooks installed)`.

| Check | How | Pass |
|---|---|---|
| Flash | H: hold the speedbreaker key / pad button (or `flash`) | H `Lights: flashing our headlights (speedbreaker bind held, 1 so far)`; H's own headlight flares grow and brighten while held and go back when released. C: `Lights: the other player flashes their headlights (1 so far)`; H's car on C's screen flashes at the same moment. Nothing slows down (`Co-op: speedbreaker switched off` may show, as before). |
| Keyboard and pad | try both binds, and a re-bound key from the game's controls menu | each flashes; releasing stops it within a frame |
| Daylight | flash where the game shows no headlight glow | the flares appear while held |
| Solo | not connected (or `FlashLights=0`) | the speedbreaker works as in the plain game, no `Lights:` flash lines |

## Story scenes (`SyncStoryScenes`)

Both PCs in free roam, linked, `TestKeys=1`, `AutoPilot=0`. `story <movie>` stands in for the career
playing a movie (getting a real one means reaching that point of the career).

| Check | How | Pass |
|---|---|---|
| Wait | `SyncStoryScenes=1` on both. H: `story storyfmv_pin11` | H plays the movie. C: banner `<name> is watching a story scene - your game waits`, log `Story: the other player is watching a story scene (...) - holding the world`; C's car, traffic and cops stand still. When H's movie ends or is skipped: C `hold released (scene over, N s)`, banner `<name> finished the story scene`, C drives again. |
| Hold limits | as above, but C is in a race / the Safe House / loading | no hold on C, only the notice (`is watching a story scene`) |
| Watch along | `SyncStoryScenes=2` on C. H: `story blacklist_15` (C's career must have reached rival 15, i.e. any normal career) | C `playing blacklist_15 here too`, the movie runs on both. `story` on C shows `in-game 1`. |
| Not reached yet | `SyncStoryScenes=2` on C, H: `story blacklist_01` while C is not at rival 1 | no movie on C (spoiler rule), C holds instead |
| Skip vote | both watching the same movie (watch along). Press skip (Enter) on H only | H `Story: voted to skip ... waiting for the other player`, overlay `Cutscene: skip voted`, movie keeps playing on both. Then skip on C: `both voted - skipping` on both, both movies close. |
| Second vote ends it | while both watch, C votes first, then H presses skip | H `both voted - skipping`; C closes its movie within about a second (`both voted` or `the other player is past it`) |
| Real career | play until a Blacklist movie / story movie / Safe House rival movie comes up | as `Wait`; for a Safe House movie the other PC waits while it roams |
| World pauses | H watches `story storyfmv_pin11` alone (C disconnected or `SyncStoryScenes=0`) | as in the unmodded game. Connected with `SyncStoryScenes=1`: H's world also stops under the movie (log `Coop: pause menu pauses again`) |

## Prologue and new profiles

Co-op stays off until a profile has started its career and finished the prologue (the scripted races
16.x in the starting BMW). This is checked on each PC (`Game::CoopAllowed`) and sent with the state
(`CarState.pad[1]`).

Making a fresh profile without losing the real one:

1. Close the game. Copy `Documents\NFS Most Wanted` to a backup folder, then rename the folder (e.g. to
   `NFS Most Wanted.real`). The game now finds no profile.
2. Set `HostIP` to the other PC. For the second side of the check, the other PC (on a normal career)
   should show `<name> is still in the prologue` once and no car.
3. Start the game focused, create the profile by hand ("Create a new alias?" Yes, the autosave notices,
   the name, "save the alias?" Yes), then Career > Start Career.
4. Afterwards close the game, rename the test folder away and the real one back. Check the real save's
   time/hash is what it was.

A pass looks like this:

| Step | Expected |
|---|---|
| Boot with no profile | log `Start: no saved profile - the profile screens are left to the player`; the mod presses nothing on the profile screens (default name `NAME` is NOT accepted for you) |
| A question on the profile screen | `Start: the profile screen asks a question (op ...) - left to the player`, nothing more is confirmed in that boot |
| Profile loaded, career not started | banner `Finish the prologue first - co-op starts in free roam` once; overlay `Co-op: off - finish the prologue first`; `Start: direct free roam - career not started in this profile` |
| Prologue races (16.1.0, 16.2.2 ...) | no `Event: sent ... to peer`, no `Start: on the grid ... waiting`, no `AI racer built as peer car`, no ghost car; the other PC shows `<name> is still in the prologue ...` once and no car |
| Next boot of that profile | the load screen is confirmed again (`OK on the load screen`), the gate notice shows again |
| Normal career profile | no `Co-op: off` lines; ghost, events, traffic as usual |

## Before a release

1. `tests\build_tests.bat` passes.
2. `solo`, `smoke`, `race`, `results`, `boss`, `restart`, `cutscene`, `pursuit`, `ram`, `together`, `kinds` pass on two PCs.
3. Save backup and restore pass.
4. One real session by two people with `TestKeys=0`: free roam, a race started from the game's own menus
   (not `event`), a pursuit, a rival race.
5. Install the release zip into a copy of the game **without** the Widescreen Fix (and once with it), start it
   through `MWTogetherLauncher.exe` > Play: the plain game's title screen is `MW_LS_Splash.fng`, the fixed
   game's `WS_MW_LS_Splash.fng`, and the fast start must get past both into free roam.
6. Update the checklist in `README.md`: a line loses its `(?)` only when it was seen working on two PCs.

## Known limits of the test tools

- The autopilot doesn't drive Challenge Series events (it stays on the start line).
- `event` starts any event regardless of career progress; real play goes through the game's menus.
- A parked car in a pursuit is busted in about 30 seconds, so pursuit scenarios are short.
- Only one copy of the game can run per PC, so co-op always needs two PCs.
