# NFS MW Together: co-op career for Need for Speed: Most Wanted (2005)

![NFS MW Together](docs/banner.png)

Play the Most Wanted career with a friend! (2 players, LAN)

A **co-op / multiplayer mod for Need for Speed: Most Wanted 2005** (PC, v1.3): two players play the
single-player **career campaign together** - free roam with traffic, races against AI opponents, the
Blacklist rivals, police pursuits, each with their own car and tuning. Works over LAN or a VPN
(Hamachi, Radmin VPN, ZeroTier).

*A.K.A.: NFS MW co-op, NFSMW multiplayer career, Most Wanted 2005 coop campaign, NFS Most
Wanted online free roam with a friend, Need for Speed Most Wanted two player mod.*

<details open><summary><h2>Install</h2></summary>


1. Download the latest zip from **[Releases](https://github.com/StCost/nfsmw-together/releases/latest)** and unpack it into your game folder, next to `speed.exe`.
2. Run `MWTogetherLauncher.exe`. Type a nickname, pick **Host LAN** or **Join LAN** (to join, enter the
   host's IP address) and press **PLAY**.
3. Do the same on the other PC. The game skips the intro, loads your career into free roam and the two of
   you connect by yourselves.
4. Updating: unpack the new zip over the old one. Your nickname and settings (`scripts\MWTogether.ini`) are kept; the zip only brings `MWTogether.default.ini` for new installs.

ATTENTION: Both players must finish the career prologue (the opening races). Reach free roam to see and interact with other players.

Don't want to replay the prologue? [Releases](https://github.com/StCost/nfsmw-together/releases/latest) also have a **StarterSave** zip: a fresh career saved at the end of the prologue: you drive
the last short scripted race, then go to the car lot and pick your first car. Unpack it into `Documents` (it holds the `NFS Most Wanted` folder; back up
yours first if you have one), start the game and pick the `MWT` profile.


</details>

<details open><summary><h2>You need</h2></summary>


- Need for Speed: Most Wanted (2005) for PC, patched to **v1.3**. Your own copy: no game files are included here.
- The same version of the mod on both PCs.
- LAN network, or a VPN like Hamachi, Radmin VPN or ZeroTier,
  or UDP port 47555 forwarded to the host.
- If one of you can't see the other, a firewall or another VPN (e.g. Surfshark) blocks it: allow UDP 47555 for `speed.exe` on that PC, or pause the other VPN. The game shows a notice when only one side receives the other.


</details>

<details open><summary><h2>What to expect</h2></summary>


- **Your own cars.** Each player drives their own car, with all its tuning and visuals, everywhere. Including damage and brake lights.
- **Events together.** Start any career event and your partner is taken along. You get the same opponents,
  start at the same moment and see each other.
- **Shared wins.** If either of you finishes first, it counts for both players: the one still driving is finished as the winner on the spot and gets the same winner's screen.
- **Police.** In free roam the cops are shared: you get the same heat, you lose them together and you get
  busted together. Your partner's cops show on your minimap and set off your radar detector.
- **One world.** Shared traffic when you're near by, collisions between your cars, blips on the minimap and map.
- **Saves.** Everyone keeps their own career save. The host stores a copy of the partner's save after every
  autosave and hands it back if theirs is ever missing. An existing save is never replaced, so everyone's own settings (camera, units, controls) stay theirs.
- **Not yet:** 4 players, and prologue together.


</details>

<details><summary><h2>Features and progress (98%)</h2></summary>


<details><summary><b>Connecting</b></summary>

- [x] Direct connection between two PCs (one hosts, one joins), with ping shown in the overlay
- [x] Clean "bye" when someone quits, Alt+F4 included, and a timeout if a game just disappears
- [x] Stays connected when a game is paused, minimized or alt-tabbed
- [x] Game keeps running in the background when alt-tabbed or paused
- [x] Status overlay on F8, and it remembers whether you hid it
- [x] Launcher (`MWTogetherLauncher.exe` next to `speed.exe`) in the game's style: name, host or join, the host's IP, start options under Settings, then Play
- [x] A short notice in the corner when the other player joins or leaves, even with the overlay hidden.
- [x] Notifications use game's own UI: the EA Trax banner (where songs are announced) shows "<name> joined / left";
- [x] Notices stay on screen for 10 seconds, the game's banner included, without sliding in again (`[UI] NoticeSeconds`)
- [x] Joining while the other player is in a race: you roam freely, get a notice, and go together into the next event (?)
- [x] Strangers can't interfere: each game ignores network packets from anyone except the player it connected to
- [x] Event starts and restarts are repeated for a few seconds, so a lost packet can't leave you in different races
- [ ] Up to 4 players


</details>
<details><summary><b>Your cars</b></summary>

- [x] Each player drives their own car, with their full tuning, in free roam and in races
- [x] Smooth movement of the other player's car (about 1-10 mm of jitter)
- [x] Spawn protection, so two cars appearing on the same spot don't collider or launch each other
- [x] Other player's blip on the minimap
- [x] Other player on the world map, coloured like your own arrow and moving while the map is open
- [x] Pick your own blip colour (launcher: My colour on the map, `[Player] Color`): the other player sees you in it on the minimap and the world map
- [x] Car-to-car collisions between players, with the impact passed on: ram your partner and their car is thrown on their screen too
- [x] Your names in the race standings instead of the shared save name
- [x] Your partner's car shows brake lights and damage from crashes (dents, broken parts)
- [x] Your partner's car shows steering and nitrous


</details>
<details><summary><b>The world</b></summary>

- [x] Shared traffic when you're near each other
- [x] Cops of the other player show up for you, light bars included
- [x] You see the cops chasing the other player
- [x] Cops may switch to chasing the other player when you're both being chased (?)
- [x] Traffic cars near the other player keep full physics instead of moving at reducing quality
- [x] Hitting the other player's traffic, cops or race opponents pushes the real car on their PC
- [x] Pause menu and world map don't pause the time while you're connected
- [x] No speedbreaker in co-op (slow motion can't work for two people)
- [x] Smooth traffic handover: when you drive away from the other player, their traffic cars around you don't vanish - they drive on as your own traffic. When you come back, your own cars stay until they're out of sight and the other player's cars only appear where you can't see them pop in (behind you or far away), or one of your nearby cars quietly takes their place (`[Traffic] SmoothHandover`, 0 = the old instant switch) (?)
- [x] Shared traffic inside races: the host's traffic cars are shown by the client's own traffic cars, so race scripts keep every car they own. Cars are in the same places on both screens, but may be a different model when no matching one is free (`[Traffic] SharedInRaces`, 0 = each player keeps their own traffic in races)
- [x] No cars popping up in front of you: the game never puts a new traffic car or cop right in front of the other player (within ~60 m, it picks another spot), and the other player's cars only appear on your screen outside your view or farther than ~60 m (`[Traffic] NoSpawnNearPlayers`, 0 = as before) (?)


</details>
<details><summary><b>Events and races</b></summary>

- [x] Starting an event takes the other player into it too, even from the results screen of the last race
- [x] Both games build the same AI opponents (car models and tuning)
- [x] Opponents on the client follow the host's opponents, so the race looks the same on both screens
- [x] Opponent names match on both PCs
- [x] Race positions use each player's real progress, and the order changes as soon as one passes the other (?)
- [x] Joining an event from far away: the other player still gets their racer in the standings (in an AI car's body if their car model isn't loaded on your PC yet)
- [x] At a race start you stand on different grid slots and see each other's car and name from the countdown on (`SeparateGridSlots`)
- [x] Start together: whoever loads first waits on the grid until the other is ready (`StartTogether`)
- [x] Restarting a race restarts it for both of you
- [x] AI opponents' catch-up (rubber banding) follows whichever player is ahead, so they race the best of you (`CatchUpLeader`). Blacklist rivals too: in a rival race the rival follows whoever of you is ahead
- [x] Race intro cutscenes play normally for both
- [x] A win by either player counts for both: cash, unlocks and career progress in each player's own career (`SharedWins`)
- [x] The other player's finish is shown in your standings
- [x] When one of you finishes first, the other is finished as the winner right away, even while still driving: 1st place, the winner's screen, the payout, and in rival races the rival's progress, each in their own career (`SharedWins`) (?)
- [x] Blacklist rival races: the rival on the client follows the host's rival, also in the second race of the series and when the two games built different cars for it
- [x] Blacklist rival races: you both race the rival in the same event (your partner's car drives alongside, outside the standings), and each of you gets the normal rival flow after winning
- [x] Rival rewards: after beating a rival, each player picks their own markers and gets their own rewards (pink slip, cash, parts) in their own career. Nothing is shared or synced here on purpose
- [x] Bounty counts for each player's own career (seen after an escape)
- [x] Shared milestones: when one of you completes a milestone or a speed trap of your Blacklist rival, the other player gets it too, with its bounty, if their career is at the same rival (`[Career] SharedMilestones`)
- [x] Challenge Series events: when you both start from the main menu, both load the event with its preset car. A player who is roaming in career isn't pulled in


</details>
<details><summary><b>Career</b></summary>

- [x] Each player keeps their own career; wins are shared, so both careers move forward together
- [x] The host keeps a copy of the other player's career save, updated after every autosave. If that player's own save is missing when they connect, it's restored from the host. An existing save is never replaced, so each player's settings stay their own (`[Career] RestoreOnlyMissing`)
- [x] Cutscene skip voting: a cutscene you both watch is skipped only when both press skip
- [x] Story scenes outside events: when one of you gets a story movie (Blacklist rival movie, story FMV, also in the Safe House) or a story cutscene in the world, the other gets a notice and their game waits in free roam until it's over (`SyncStoryScenes`)
- [x] With `SyncStoryScenes=2` the same story movie also plays on the other PC if their career has got that far, and a movie you both watch is skipped only when both press skip (in-world cutscenes are never started on the other PC)
- [x] Shared pursuits in free roam (`[Coop] SharedPursuits`, on by default): when one of you gets heat the other gets the same, and the hotter of the two counts
- [x] The other player's cops are on your minimap while they chase, and your radar detector beeps for them like for your own
- [x] Ramming one of the other player's cops starts a pursuit on you, like hitting one of your own
- [x] Busted together: if one of you gets busted, the other one is busted too, through the game's own arrest (?)
- [x] Escape together: the cooldown only runs while neither of you is seen, starts over for both when either one is spotted, and you both get away at the same moment
- [x] No pursuit-start camera in co-op: the camera stays on your car when a chase begins (`NoPursuitCams`, launcher: No pursuit start camera)
- [x] With `SharedPursuits=0` it's like before: getting busted or escaping only affects the player being chased


</details>
<details><summary><b>Quality of life (settings in <code>[Start]</code>, <code>[UI]</code> and <code>[Input]</code>)</b></summary>

- [x] Load straight into career free roam after the profile (`DirectFreeRoam`); optionally skip the intro logos and movie (`SkipIntro`, off by default)
- [x] The "load complete" screen after the title confirms itself (`SkipLoadComplete`, on by default)
- [x] No "leave the safe house?" question (`SkipLeaveSafeHouseDialog`)
- [x] Menus react while their opening animation plays, and animate 2x faster (`IntroInput`, `MenuSpeed`) to improve UI responsivness
- [x] Optional: close the pursuit result screens in free roam by itself ("Bounty achieved" and the pursuit summary), off by default (`SkipPursuitResults`). Getting busted still shows the fine screen
- [x] A gamepad or wheel plugged in while the game is running (USB or Bluetooth) works right away, and so does one that dropped out and came back; your bindings stay as they were (`[Input] GamepadHotplug`, launcher: Detect gamepads connected while playing) (?)
- [x] Starting the game without your pad no longer resets its controller bindings to the defaults (`[Input] KeepPadBindings`) (?)


</details>
<details><summary><b>Test tools (<code>[Debug] TestKeys=1</code>)</b></summary>

- [x] **F5** autopilot: the game's AI drives your car, cruising in free roam and following the route in races. `AutoPilot=1` turns it on by itself. For testing purposes
- [x] Autopilot uses nitrous on straights (`[Debug] AutoPilotNos`, default on)
- [x] `[Window] StartInBackground=1`: the game starts without taking the focus, loads the profile and goes into free roam in the background, and stays muted until you click it. For testing purposes
- [x] Performance diagnostics: `[Debug] Perf=1` logs fps, 1% lows, worst frame and how many milliseconds each part of the mod takes, every 5 seconds (it's also shown in the F8 overlay). For testing purposes
- [x] **F7** starts the test event (`TestEvent`) for both players. For testing purposes
- [x] **F10 / F11** spawn or remove a test copy of the other player's car, **F12** spawns a test cop. For testing purposes
- [x] Test commands in `scripts\MWTogether.cmd` also include `pursuit` (the cops start a pursuit on you), `uncop` (remove every police car, so the pursuit ends in an escape), `ram cop` (drive into the other player's nearest cop) and `tp x y z` (move your car). Full list in `docs/TESTING.md`. For testing purposes


</details>

[^1]: (?) means feature is implemented, unit tested, but not playtested 100%.


</details>

<details><summary><h2>Configuration</h2></summary>

Every option is in `scripts\MWTogether.ini`, which comments each one. Main sections:
`[Network]`, `[Player]`, `[Ghost]`, `[Traffic]`, `[Coop]`, `[Start]`, `[AutoStart]`, `[UI]`, `[Window]`, `[Debug]`.


</details>

<details><summary><h2>Build from source</h2></summary>

- Visual Studio 2022 with the C++ workload (MSVC x86).
- Run `build.bat`. It produces `build\MWTogether.asi`. When this folder sits inside the game folder
  (`<game>\MWTogether-src\`), the script also installs the plugin into `<game>\scripts\`.
- Nothing builds automatically on GitHub. The **Build** workflow under **Actions** runs only when you start it
  by hand: it builds the plugin and the launcher, runs the tests and packages the zip with the ASI loader. Give
  it a version number and it also creates the GitHub release.
- Local release, without Actions: `tools\release.ps1 -Version 0.2.0` publishes a pre-release; add `-Final` for a
  normal release. `-DryRun` only builds the zip in `dist\`.


</details>

<details><summary><h2>Testing</h2></summary>

The network code, the config reader and the maths behind the remote cars have tests that run without the game:

```
tests\build_tests.bat
```

It builds `build\tests.exe` and runs it, then runs the same tests again under AddressSanitizer when your
MSVC supports it. Tests include: packet layouts and protocol version, connecting / leaving / timeouts,
snapshot interpolation and extrapolation, clock sync, traffic / event / roster / restart packets, `MWTogether.ini`
parsing, and fuzz tests that throw several hundred thousand random and damaged datagrams at the packet
handler to make sure nothing a network can deliver crashes the game. Exit code 0 means everything passed.

Everything that needs the game - the test commands, the two-PC scenarios (`tools\coop-test.ps1`) and what a pass looks like - is in [docs/TESTING.md](docs/TESTING.md).

What the tests can't cover is everything inside the game itself (spawning cars, races, cops): that still needs
two PCs. See [CONTRIBUTING.md](CONTRIBUTING.md) for the details and for adding tests.


</details>

<details><summary><h2>How it works (short)</h2></summary>

`MWTogether.asi` is loaded by the ASI loader inside `speed.exe`. It hooks Direct3D 9 `Present` to run once per
frame on the game thread, and it patches game code and vtables at known v1.3 addresses, found by reverse
engineering. Remote cars are real game vehicles (`PVehicle`) whose rigid bodies are set every frame from
interpolated network snapshots. Race opponents and traffic of the other PC are "mirrors" driven the same way.
The code is in `src/`:

| File | What |
|---|---|
| `main.cpp` | frame loop, hotkeys, overlay text, start-up patches |
| `net.*` | UDP protocol (hello, state, car info, traffic, events, roster, restart, keep-alive) |
| `netutil.h` | pure helpers: interpolation maths, traffic ids, race traffic binding, traffic handover rules (camera view, when a car may appear or go), name limits, validation of received numbers |
| `game.cpp` | vehicles, spawning, the other player's car (ghost), race integration, autopilot |
| `traffic.inl` | traffic, cop and race-opponent mirroring |
| `event.inl` | shared event start, roster, co-op rules (speedbreaker, no pause, restart sync) |
| `pursuit.inl` | cops chasing the other player, shared heat / busted / cooldown, their cops on the minimap and radar, no pursuit-start camera |
| `pursuitutil.h` | the shared-pursuit rules that don't need the game (heat, cooldown, busted), tested in `tests\test_pursuit.cpp` |
| `stream.inl` | world streaming around the other player |
| `minimap.inl`, `worldmap.inl` | player blips |
| `boot.inl` | fast start (skip intro, direct free roam, no safe-house dialog) |
| `input.*` | DirectInput observation, menu automation (AutoStart) |
| `overlay.*` | D3D9 hooks and the status overlay |


</details>

<details><summary><h2>Contributing</h2></summary>

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).


</details>

<details><summary><h2>Credits</h2></summary>

- Reverse-engineering references: NFSPluginSDK (MW05 headers), MostWantedTimeTrialGhosts and
  nya-common-nfsmw (MPL-2.0), ExOpts.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG.
- Need for Speed and Most Wanted are trademarks of Electronic Arts. This is an unofficial fan project, not affiliated with EA.


</details>
