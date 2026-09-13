# Slippi Blocklist

Skip specific players in **Slippi Unranked** matchmaking by connect code.
A drop-in add-on for Slippi Dolphin on Windows; no rebuilt Dolphin, no launcher
changes, no admin rights.

* **One file.** `dinput8.dll` goes next to `Slippi Dolphin.exe`. Uninstalling
  means deleting it again.
* **Automatic.** When the matchmaking server pairs you with someone on your
  list, Dolphin never connects to them and searches again by itself. You hear a
  short beep; the other player just sees a slightly longer search.
* **Unranked only by default.** Teams and Party can be enabled. Ranked and
  Direct are never filtered.
* **Blocklist Manager.** A small window for adding and removing codes, including
  a "Recent opponents" picker that reads names and codes from your replays.

## Install guide

Requirements: Windows 10/11, Slippi Launcher, and at least one online session
played (so the Launcher has downloaded Dolphin).

1. Open the [Releases page](../../releases/latest). Under **Assets**, download
   `SlippiBlocklist-x.y.z.zip` (not "Source code"). Right-click the zip,
   choose **Extract All...**, and open the extracted folder.
2. Close Slippi Dolphin if it is open. The Slippi Launcher itself can stay open.
3. Double-click **`Install.cmd`**. If Windows shows "Windows protected your PC",
   choose *More info* and then *Run anyway*; the DLL is not code-signed.
   A black window reports what it did and ends with "Done".
4. The Blocklist Manager opens. Type a connect code such as `ABCD#123` and press
   **Add**, or click **Recent opponents...** and block from the list.

Then queue Unranked as usual.

Adding someone later: open **`Blocklist Manager.cmd`** from the extracted folder
at any time, even while Dolphin is running. Changes are saved immediately and
apply the next time you press Search. **Recent opponents...** lists the people
from your latest replays, newest first, so you can block someone right after
playing them.

If you downloaded the repository source instead of the release zip, the same
`Install.cmd`, `Uninstall.cmd` and `Blocklist Manager.cmd` are in the top
folder and forward to the files in `dist`.

To remove the add-on, double-click **`Uninstall.cmd`**. Your blocklist file is
kept.

To verify it is active, play online once and open
`%APPDATA%\Slippi Launcher\netplay\User\Slippi\blocklist.log`. The first lines
should read `hooks: WSARecvFrom=ok WSASendTo=ok`, every match appears as
`match found (unranked): ...`, and skips appear as `SKIP: ...`.

## Managing the list

`Blocklist Manager.cmd` opens the window. The same script also works from a
terminal:

```
powershell -File BlocklistManager.ps1 -List
powershell -File BlocklistManager.ps1 -Add ABCD#123 -Note "reason"
powershell -File BlocklistManager.ps1 -Remove ABCD#123
powershell -File BlocklistManager.ps1 -Recent -Count 20
powershell -File BlocklistManager.ps1 -Mode unranked,teams
powershell -File BlocklistManager.ps1 -Disable
```

The list lives next to `user.json` at
`%APPDATA%\Slippi Launcher\netplay\User\Slippi\blocklist.json`
(`netplay-beta` for the mainline beta). Edits are picked up the next time you
press Search; no restart is needed.

```json
{
  "enabled": true,
  "modes": ["unranked"],
  "beepOnSkip": true,
  "blocked": [
    "ABCD#123",
    { "code": "WXYZ#9", "note": "optional note" }
  ]
}
```

* `modes`: any of `unranked`, `teams`, `party`. `ranked` and `direct` are ignored.
* Codes are case-insensitive, and the in-game full-width `＃` is accepted.
* Activity is logged to `blocklist.log` in the same folder.

## How it works

```
Slippi Dolphin ──ENet/UDP──> mm.slippi.gg:43113   "create-ticket"   (mode, own code)
Slippi Dolphin <──ENet/UDP── mm.slippi.gg:43113   "get-ticket-resp" (players: codes, IP:port)
Slippi Dolphin ──ENet/UDP──> opponent IP:port     direct connection for the match
```

Dolphin imports `dinput8.dll` for controller support, so a copy placed in its
folder is loaded at startup. This copy forwards every DirectInput call to the
real `C:\Windows\System32\dinput8.dll` and additionally patches Dolphin's import
table entries for the two Winsock functions ENet uses (`WSARecvFrom` and
`WSASendTo`). From then on:

1. Datagrams exchanged with the matchmaking server are **read**, never modified.
   ENet packets are walked, fragmented messages reassembled, and the JSON parsed.
2. When a `get-ticket-resp` arrives for a filtered mode, each remote player's
   connect code is checked against `blocklist.json`. A hit records that player's
   `ipAddress` and `ipAddressLan` for 30 seconds.
3. Any datagram to or from a recorded address is silently dropped. Dolphin's
   peer connection therefore times out after 8 seconds and Dolphin's own
   "Connection attempt failed, looking for someone else" path requests a new
   match.

Because nothing on the wire is altered and the retry is Dolphin's own, the
add-on works with both the current `Slippi Dolphin.exe` (Ishiiruka) and the
mainline `Slippi_Dolphin.exe` beta, and keeps working across Slippi updates as
long as the matchmaking messages keep their field names.

### Repository layout

| Path | Contents |
| --- | --- |
| `hook/BlocklistHookCore.h` | ENet packet walker, fragment reassembly, match parsing, block decisions. Portable C++, no Windows headers. |
| `hook/dllmain.cpp` | The `dinput8.dll`: DirectInput forwarding, import-table hooks, config and log handling. |
| `hook/build.ps1` | Builds `dist/dinput8.dll` with MinGW-w64; `-Test` also runs the C++ tests. |
| `dolphin/SlippiBlocklist.h` | `blocklist.json` parser and code normalization, shared by the DLL and the source patch. |
| `dist/` | The end-user package: installer, uninstaller, Blocklist Manager, template config. |
| `patches/`, `scripts/patch_dolphin.py` | Alternative implementation compiled into Dolphin itself (see below). |
| `tests/` | C++ unit tests, an in-process UDP loopback test that loads the real DLL, and a PowerShell suite for the installer and manager. |

## Building from source

Requirements: MinGW-w64 `g++` (for example via MSYS2:
`pacman -S mingw-w64-x86_64-gcc`). Visual Studio and the Dolphin source tree are
not needed.

```
powershell -ExecutionPolicy Bypass -File hook\build.ps1 -Test   # builds dist\dinput8.dll and runs the C++ tests
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1     # everything, including installer and manager tests
```

Test coverage:

* `tests/test_blocklist.cpp`: config parsing, normalization, mode gating.
* `tests/test_hook_core.cpp`: ENet walking including out-of-order and duplicate
  fragments, session resets and malformed metadata; the full skip flow for
  unranked, ranked, direct, teams, self, disabled config and matchId fallback.
* `tests/test_hook_integration.cpp`: loads the built `dinput8.dll` into a process
  that imports Winsock the way Dolphin does, plays a fake matchmaking server and
  opponents over UDP loopback, and checks DirectInput forwarding, hook
  installation, skipping on a fragmented assignment, dropping in both
  directions, unaffected server and third-party traffic, ranked exemption and
  live config reload.
* `tests/run_tests.ps1`: installer and uninstaller against a throwaway launcher
  folder, manager CLI behaviour, file-format compatibility with the C++ parser,
  and replay parsing.

## Alternative: compiling the feature into Dolphin

`patches/` and `scripts/patch_dolphin.py` add the same blocklist to the Slippi
Dolphin source, with an on-screen message and an immediate re-queue instead of
the 8-second timeout:

```
python scripts/patch_dolphin.py --fork ishiiruka --repo path\to\Ishiiruka
python scripts/patch_dolphin.py --fork mainline  --repo path\to\dolphin
```

The patch adds a check to `SlippiMatchmaking::handleMatchmaking` right after
the player list is parsed; a blocked assignment drops the matchmaking
connection and returns the state machine to `INITIALIZING`, the same thing
Dolphin does when a peer connection fails. This requires a full Dolphin build
and must be redone for every Slippi release, which is why the DLL is the
default.

## Limitations

* Slippi Launcher may replace the Dolphin folder when it updates. If skipping
  stops after an update, run `Install.cmd` again.
* The DLL is not code-signed, so SmartScreen may warn on first run.
* Teams: Dolphin does not automatically retry a failed teams connection, so a
  skip shows Dolphin's usual "Could not connect to players" message.
* The skipped player experiences a failed connection followed by their own
  automatic re-search. This is inherent to any client-side block.
* Windows only.

## Acknowledgements

* [Preflight](https://www.patreon.com/rwing_aitch/posts/announcing-while-166681873)
  by Aitch, whose connect-code block feature this reproduces.
* [slpblist](https://github.com/itsonlyMiRE/slpblist), an earlier alert-based approach.
* [Slippi replay specification](https://github.com/project-slippi/slippi-wiki/blob/master/SPEC.md).
* [Slippi Dolphin matchmaking source](https://github.com/project-slippi/Ishiiruka/blob/slippi/Source/Core/Core/Slippi/SlippiMatchmaking.cpp).

## License

See [LICENSE](LICENSE).
