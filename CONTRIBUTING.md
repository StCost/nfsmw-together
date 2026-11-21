# Contributing to NFS MW Together

Thanks for helping! This is a reverse-engineering-heavy project, so a few rules keep it stable.

## Ground rules
- **Never commit game files**, saves or anything from the game install. Only our own source, docs and tools.
- **v1.3 only.** Every address is for `speed.exe` v1.3. Patch only after checking the expected bytes
  (`Mem::PatchBytes`) or the expected old call target or vtable value (`PatchCallChecked`). If it doesn't
  match, log it and leave the feature off. Never crash.
- **Document every address.** Put a comment block at the top of the feature file with each address, its
  calling convention and what it does, and how you found it (constructor, string reference, call site).
- **Game thread only.** Game calls happen from the frame callback. The only other thread is the network
  keep-alive, which touches nothing in the game.
- **Guard game calls** with `__try/__except`. Keep `std::string` and other objects with destructors out of
  `__try` functions (C2712): split into a small "Safe" helper.
- **Both players see the same world.** A feature isn't done until it is tested on two PCs. Describe the test
  in the pull request: what you did on each PC and the relevant `scripts\MWTogether.log` lines.
- **Protocol changes:** bump `COOP_VERSION` in `src/net.h` whenever a packet layout changes. Add new
  packet types at the end, with a length check in the receiver.

## Workflow
1. Fork or branch from `main`: `feature/<name>` or `fix/<name>`.
2. Build with `build.bat`.
3. Run the tests: `tests\build_tests.bat` (see below). They must pass.
4. Test on two PCs (LAN or VPN), host and client.
5. Open a pull request with what changed, how you tested it, and any new addresses with their source.

## Tests
The tests run without the game. They cover everything that doesn't need `speed.exe`: the wire format, the
packet handler, snapshot interpolation, clock sync, connection handling, config parsing and the pure helpers.

```
tests\build_tests.bat            build and run everything
tests\build_tests.bat noasan     skip the AddressSanitizer pass (faster)
tests\build_tests.bat build      only build
tests\build_tests.bat analyze    MSVC static analysis of the plugin sources (report only)
build\tests.exe fuzz             run only the tests whose name contains "fuzz"
build\tests.exe --list           list the tests
```

- `build\tests.exe` is built with run-time checks (`/RTC1`) and the debug CRT. `build\tests_asan.exe` is the
  same suite under AddressSanitizer; the script skips it with a note if your MSVC can't build it for x86.
  Run the ASan binary through the script (it needs the ASan runtime DLL from the MSVC folder).
- The exit code is 0 only if every test passed. Nothing opens a dialog box: a crash, a failed run-time check
  or a refused CRT call is printed and fails the run.
- The fuzz tests feed several hundred thousand random, truncated and bit-flipped datagrams to the packet
  handler. Each datagram ends at a no-access memory page, so reading one byte too far is caught at once.
  They are deterministic; `set MWT_FUZZ_SEED=123` tries another sequence and `set MWT_FUZZ_ITERS=2000000`
  a longer one. A failure prints the seed and the iteration.
- The loopback test opens UDP sockets on `127.0.0.1` only and takes about 8 seconds (it waits for the
  keep-alive thread).

What lives where:

| File | What |
|---|---|
| `tests/mwt_test.h`, `test_main.cpp` | the test runner (`TEST`, `CHECK`, `CHECK_EQ`, `CHECK_NEAR`, `CHECK_STR`, `REQUIRE`) |
| `tests/net_helpers.h` | a clean `Net` per test, fake clocks, packet builders, capture of sent packets |
| `test_protocol.cpp` | struct sizes and offsets, magic, `COOP_VERSION`, packet type numbers |
| `test_net.cpp` | hello / bye / timeouts, sequence numbers, snapshot ring, interpolation, clock offset, car info |
| `test_packets.cpp` | traffic, event, roster, racer name, restart: real serializer -> handler round trips |
| `test_file.cpp` | the file transfer (lost / repeated / damaged chunks, request, "no file") and `src/saveutil.h`: names from the network, which save wins, safe file writes |
| `test_util.cpp` | `src/netutil.h`: name truncation, traffic ids, race traffic binding, Hermite / nlerp, float validation |
| `test_pursuit.cpp` | `src/pursuitutil.h`: shared heat, cooldown hold / delay, busted together, the pursuit fields of the state packet |
| `test_config.cpp` | `MWTogether.ini` defaults, parsing, clamping; `perf.h` |
| `test_fuzz.cpp` | random / mutated / truncated datagrams |
| `test_loopback.cpp` | the real socket code against a raw UDP socket on localhost |

Rules for new code:
- **Keep logic that doesn't need the game out of `game.cpp`.** Put it in a header like `src/netutil.h` (or in
  `net.cpp` / `config.cpp`) and add a test. The test build compiles `net.cpp`, `config.cpp` and `log.cpp` with
  `/DMWT_TESTS`, which adds the `Net::Test_*` hooks declared at the end of `src/net.h`; the plugin build
  doesn't contain them.
- **A new packet type** needs: a size check in `test_protocol.cpp`, a round trip in `test_packets.cpp`, and an
  entry in the type list of `test_fuzz.cpp`. If a layout changes, bump `COOP_VERSION` and the pinned
  numbers in `test_protocol.cpp` together.
- **Never trust the network.** Check the length before reading a packet, terminate strings yourself, and
  reject numbers the game can't take (`NetUtil::ValidCarState`). The fuzz tests will find what you miss.
- A bug fix in testable code comes with a test that fails without the fix.

## CI
There is no automatic CI. The **Build** workflow (`.github/workflows/build.yml`) only runs when someone starts
it by hand from the Actions tab. It builds the plugin, runs `tests\build_tests.bat`, compiles the launcher,
prints the static-analysis report (never fails the run) and packages the zip; with a version number it also
creates the release. Running it is optional: `build.bat` and `tests\build_tests.bat` on your PC check the same.

## Reporting bugs
Open an issue and include:
- `scripts\MWTogether.log` from **both** PCs,
- which PC was host and which was client,
- what you did, what you expected and what happened instead.

## Good first areas
The unticked lines in the [README checklist](README.md#features-and-progress) are what's left: more than two players, internet play without a VPN, name tags, and story cutscenes for both players.

## Testing in the game
See [docs/TESTING.md](docs/TESTING.md): the test commands, `tools\coop-test.ps1` scenarios for two PCs, and the release checklist. Add a scenario there when you add a co-op feature.
