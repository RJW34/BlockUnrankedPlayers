# Slippi connect-code blocklist

Skip specific players in **Slippi Unranked** without touching anything else about
your Slippi setup. This is the "block up to N connect codes" feature from the
Preflight mod, as a standalone add-on you can hand to friends.

* **Drop-in.** One file (`dinput8.dll`) goes next to `Slippi Dolphin.exe`. No
  rebuilt Dolphin, no launcher changes, no admin rights. Uninstall = delete it.
* **Automatic.** When the matchmaking server pairs you with someone on your list,
  Dolphin never connects to them and searches again by itself. You hear a beep
  and the other person just sees a slightly longer search.
* **Unranked (and optionally Teams / Party) only.** Ranked and Direct are never
  filtered, by design.
* **Managed with a small window** (`Blocklist Manager.cmd`) that can also pick
  from the people you played most recently, read straight from your replays.

## For your friends: install in 30 seconds

1. Download the release zip and extract it anywhere.
2. Close Slippi Dolphin if it is open.
3. Double-click **`Install.cmd`**.
4. The Blocklist Manager opens. Type a connect code (`ABCD#123`) and press Add,
   or click **Recent opponents...** and block from the list.

That's it. Queue Unranked as usual. `Uninstall.cmd` puts everything back.

The zip is the contents of `dist/` after running `hook\build.ps1` once (see
below); it contains `dinput8.dll`, the installer, the manager, and `README.txt`.

## How it works

```
Slippi Dolphin ──ENet/UDP──> mm.slippi.gg:43113   "create-ticket" (mode, my code)
Slippi Dolphin <──ENet/UDP── mm.slippi.gg:43113   "get-ticket-resp" (players: codes + IP:port)
Slippi Dolphin ──ENet/UDP──> opponent IP:port     direct connection for the match
```

`dinput8.dll` is loaded by Dolphin at startup because Dolphin imports DirectInput
for controllers. Our copy forwards every DirectInput call to the real
`C:\Windows\System32\dinput8.dll`, and additionally patches Dolphin's import
table entries for the two Winsock functions ENet uses (`WSARecvFrom`,
`WSASendTo`). From then on:

1. Datagrams to/from the matchmaking server are **read** (never modified). The
   ENet packets are walked, fragmented messages reassembled, and the JSON parsed.
2. On a `get-ticket-resp` in a filtered mode, every remote player's connect code
   is checked against `blocklist.json`. A hit records that player's
   `ipAddress` / `ipAddressLan` for 30 seconds.
3. Any datagram to or from a recorded address is silently dropped (sends report
   success, receives report "nothing"). Dolphin's peer connection therefore
   times out after 8 seconds and its own code path
   `"Connection attempt failed, looking for someone else"` requests a new match.

Because nothing on the wire is altered and the retry is Dolphin's own, this
works on both the current `Slippi Dolphin.exe` (Ishiiruka) and the mainline
`Slippi_Dolphin.exe` beta, and keeps working across Slippi updates as long as
the matchmaking JSON keeps its field names.

### Files

| Path | What |
| --- | --- |
| `hook/BlocklistHookCore.h` | ENet packet walker, fragment reassembly, match parsing, block decisions. Pure C++, no Windows headers. |
| `hook/dllmain.cpp` | The `dinput8.dll`: DirectInput forwarding, import-table hooks, config/log files. |
| `hook/build.ps1` | Builds `dist/dinput8.dll` with MinGW-w64 (`-Test` also runs every C++ test). |
| `dolphin/SlippiBlocklist.h` | `blocklist.json` parser and code normalization, shared by the DLL and the source patch. |
| `dist/` | The end-user package: installer, uninstaller, Blocklist Manager, template config. |
| `patches/`, `scripts/patch_dolphin.py` | Alternative: the same feature built into Dolphin itself (see below). |
| `tests/` | C++ unit tests, an in-process UDP loopback test that loads the real DLL, and a PowerShell suite for the installer and manager. |

### `blocklist.json`

Lives next to `user.json`:
`%APPDATA%\Slippi Launcher\netplay\User\Slippi\blocklist.json`
(`netplay-beta` for the mainline beta). Edits are picked up the next time you
press Search; no restart needed.

```json
{
  "enabled": true,
  "modes": ["unranked"],
  "requeueDelayMs": 1000,
  "beepOnSkip": true,
  "blocked": [
    "ABCD#123",
    { "code": "WXYZ#9", "note": "rage quits every set" }
  ]
}
```

* `modes`: any of `unranked`, `teams`, `party`. `ranked` and `direct` are ignored.
* `requeueDelayMs`: only used by the source-patch variant.
* Codes are case-insensitive and the in-game full-width `＃` is accepted.
* A log of what happened is written next to it as `blocklist.log`.

### Blocklist Manager

`Blocklist Manager.cmd` opens a window (Windows PowerShell, nothing to install).
The same script works from a terminal:

```
powershell -File dist\BlocklistManager.ps1 -List
powershell -File dist\BlocklistManager.ps1 -Add ABCD#123 -Note "why"
powershell -File dist\BlocklistManager.ps1 -Remove ABCD#123
powershell -File dist\BlocklistManager.ps1 -Recent -Count 20
powershell -File dist\BlocklistManager.ps1 -Mode unranked,teams
powershell -File dist\BlocklistManager.ps1 -Disable
```

`-Recent` reads the Game Start block of your newest `.slp` replays (connect
codes at offset `0x221`, names at `0x1A5`), drops your own code, and lists who
you played, newest first.

## Building the DLL and running the tests

You need MinGW-w64 `g++` (for example MSYS2: `pacman -S mingw-w64-x86_64-gcc`).
No Visual Studio, no Dolphin source tree.

```
powershell -ExecutionPolicy Bypass -File hook\build.ps1 -Test   # builds dist\dinput8.dll + C++ tests
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1     # everything, incl. installer + manager
```

What the tests cover:

* `tests/test_blocklist.cpp`: config parsing, normalization, mode gating, backoff.
* `tests/test_hook_core.cpp`: ENet walking incl. out-of-order and duplicate
  fragments, connect resets, bogus metadata; the full skip flow for unranked,
  ranked, direct, teams, self, disabled config, matchId fallback.
* `tests/test_hook_integration.cpp`: loads the built `dinput8.dll` into a process
  that imports Winsock like Dolphin does, plays fake matchmaking server and
  opponents over UDP loopback (`127.0.0.1/2/3`), and checks that DirectInput is
  forwarded, hooks are installed, a fragmented assignment triggers a skip,
  traffic with the blocked peer is dropped in both directions, the server and
  other peers are untouched, ranked is never filtered, and editing
  `blocklist.json` takes effect live.
* `tests/run_tests.ps1`: installer/uninstaller against a throwaway launcher
  folder (fresh install, re-install, foreign `dinput8.dll` backup/restore,
  missing launcher), manager CLI (add/remove/normalize/reject own code/modes),
  file format compatibility, and replay parsing on real replays if present.

## Verifying it end to end

The tests exercise everything except the real Slippi server, and a smoke run of
the real `Slippi Dolphin.exe` (v3.6.4) with the DLL confirmed it loads, forwards
DirectInput, installs both hooks and parses the config. To see a skip happen for
real without bothering strangers: block a friend's code, both queue Unranked at
the same time (off-peak, so the server pairs you), and read `blocklist.log`. When
the server pairs you, the log shows `SKIP: <code> ...` followed roughly 8 seconds
later by a new `create-ticket sent`.

## Alternative: build it into Dolphin

If you would rather have the feature compiled in (with an on-screen message and
an immediate re-queue instead of the 8-second timeout), `patches/` and
`scripts/patch_dolphin.py` add the same blocklist to the Dolphin source:

```
python scripts/patch_dolphin.py --fork ishiiruka --repo path\to\Ishiiruka   # v3.6.4 or slippi branch
python scripts/patch_dolphin.py --fork mainline  --repo path\to\dolphin     # v4.0.0-mainline-beta.19 or slippi branch
```

It splices a check into `SlippiMatchmaking::handleMatchmaking` right after the
players are parsed; a blocked assignment drops the matchmaking connection and
returns the state machine to `INITIALIZING`, exactly what Dolphin already does
when a peer connection fails. This needs a full Dolphin build and has to be
redone for every Slippi release, which is why the DLL is the default.

## Caveats

* Slippi Launcher updates replace the Dolphin folder; if skipping stops after an
  update, run `Install.cmd` again.
* Windows SmartScreen may warn about the unsigned DLL/zip the first time.
* Teams: Dolphin does not auto-retry a failed teams connection; you get its
  "Could not connect to players" error and press Search again.
* The other player experiences your skip as a failed connection followed by
  their own automatic re-search. This is inherent to any client-side block,
  including Preflight's.

## Credits and references

* Preflight (the mod this copies one feature of) by Aitch:
  https://www.patreon.com/rwing_aitch/posts/announcing-while-166681873
* slpblist, an earlier sound-alert approach: https://github.com/itsonlyMiRE/slpblist
* Slippi replay spec: https://github.com/project-slippi/slippi-wiki/blob/master/SPEC.md
* Slippi Dolphin matchmaking source: https://github.com/project-slippi/Ishiiruka/blob/slippi/Source/Core/Core/Slippi/SlippiMatchmaking.cpp
