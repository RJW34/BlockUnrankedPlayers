#!/usr/bin/env python3
"""Apply the connect-code blocklist feature to a Slippi Dolphin checkout.

Works on both forks:
  * ishiiruka  - project-slippi/Ishiiruka  (the "Slippi Dolphin.exe" the launcher
                 ships in netplay/)
  * mainline   - project-slippi/dolphin    (the Qt build in netplay-beta/)

It copies dolphin/SlippiBlocklist.h into Source/Core/Core/Slippi/ and splices a
few small blocks into SlippiMatchmaking.{h,cpp}. Every splice is anchored on an
exact, unique piece of upstream text, so if upstream moves things around the
script fails loudly instead of producing a half-patched tree.

Usage:
    python scripts/patch_dolphin.py --fork ishiiruka --repo path/to/Ishiiruka
    python scripts/patch_dolphin.py --fork mainline  --repo path/to/dolphin
    python scripts/patch_dolphin.py --fork ishiiruka --repo ... --check   # dry run
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
HEADER_SRC = HERE.parent / "dolphin" / "SlippiBlocklist.h"
SLIPPI_DIR = Path("Source/Core/Core/Slippi")
MARKER = "SlippiBlocklist"  # presence of this in a file means "already patched"


# --------------------------------------------------------------------------- #
# Ishiiruka (tabs, camelCase members, printf-style logging)
# --------------------------------------------------------------------------- #
ISHIIRUKA = {
    "SlippiMatchmaking.h": [
        (
            "#include <json.hpp>\n\nusing json = nlohmann::json;\n",
            "#include <json.hpp>\n\n"
            '#include "Core/Slippi/SlippiBlocklist.h"\n\n'
            "using json = nlohmann::json;\n",
        ),
        (
            "\tbool m_isHost;\n",
            "\tbool m_isHost;\n"
            "\n"
            "\t// Local connect-code blocklist (User/Slippi/blocklist.json). Reloaded on every search.\n"
            "\tSlippiBlocklist::Config m_blocklist;\n"
            "\tint m_blocklistSkips = 0;\n",
        ),
        (
            "\tvoid handleConnecting();\n};",
            "\tvoid handleConnecting();\n"
            "\n"
            "\tvoid loadBlocklist();\n"
            "\tbool shouldSkipMatchForBlocklist();\n"
            "};",
        ),
    ],
    "SlippiMatchmaking.cpp": [
        (
            '#include "Common/StringUtil.h"\n',
            '#include "Common/StringUtil.h"\n'
            '#include "Common/CommonPaths.h"\n'
            '#include "Common/FileUtil.h"\n'
            '#include "Core/Slippi/SlippiBlocklist.h"\n'
            '#include "VideoCommon/OnScreenDisplay.h"\n',
        ),
        (
            "void SlippiMatchmaking::MatchmakeThread()\n{\n",
            "void SlippiMatchmaking::MatchmakeThread()\n{\n"
            "\t// Pick up edits to blocklist.json without restarting Dolphin\n"
            "\tloadBlocklist();\n"
            "\n",
        ),
        (
            '\tm_isHost = getResp.value("isHost", false);\n',
            '\tm_isHost = getResp.value("isHost", false);\n'
            "\n"
            "\t// Local blocklist: if someone in this assignment is blocked, throw it away and ask\n"
            "\t// for a fresh ticket instead of connecting to them. This is the same path the\n"
            "\t// client already takes when a peer connection fails (see handleConnecting).\n"
            "\tif (shouldSkipMatchForBlocklist())\n"
            "\t{\n"
            "\t\tterminateMmConnection();\n"
            "\t\tm_remoteIps.clear();\n"
            "\t\tm_playerInfo.clear();\n"
            "\n"
            "\t\tint delayMs = SlippiBlocklist::RequeueDelayForSkip(m_blocklist, m_blocklistSkips);\n"
            "\t\tif (delayMs > 0)\n"
            "\t\t\tCommon::SleepCurrentThread(delayMs);\n"
            "\n"
            "\t\t// Matchmaking may have been cancelled while we were waiting\n"
            "\t\tif (m_state != ProcessState::MATCHMAKING)\n"
            "\t\t\treturn;\n"
            "\n"
            "\t\tm_state = ProcessState::INITIALIZING;\n"
            "\t\treturn;\n"
            "\t}\n",
        ),
        (
            "int SlippiMatchmaking::LocalPlayerIndex()\n",
            "void SlippiMatchmaking::loadBlocklist()\n"
            "{\n"
            "\tm_blocklist = SlippiBlocklist::Config();\n"
            "\tm_blocklistSkips = 0;\n"
            "\n"
            "\tstd::string folder = File::GetSlippiUserConfigFolder();\n"
            "\tif (!folder.empty() && folder.back() != DIR_SEP_CHR && folder.back() != '\\\\')\n"
            "\t\tfolder += DIR_SEP;\n"
            "\tstd::string path = folder + SlippiBlocklist::FILE_NAME;\n"
            "\n"
            "\tif (!File::Exists(path))\n"
            "\t{\n"
            '\t\tINFO_LOG(SLIPPI_ONLINE, "[Blocklist] No %s, nothing will be filtered", path.c_str());\n'
            "\t\treturn;\n"
            "\t}\n"
            "\n"
            "\tstd::string contents;\n"
            "\tif (!File::ReadFileToString(path, contents))\n"
            "\t{\n"
            '\t\tERROR_LOG(SLIPPI_ONLINE, "[Blocklist] Could not read %s", path.c_str());\n'
            "\t\treturn;\n"
            "\t}\n"
            "\n"
            "\tstd::string err;\n"
            "\tm_blocklist = SlippiBlocklist::Parse(contents, &err);\n"
            "\tif (!err.empty())\n"
            "\t{\n"
            '\t\tERROR_LOG(SLIPPI_ONLINE, "[Blocklist] Failed to parse %s: %s", path.c_str(), err.c_str());\n'
            '\t\tOSD::AddMessage("Blocklist: " + err, OSD::Duration::VERY_LONG, OSD::Color::RED);\n'
            "\t\treturn;\n"
            "\t}\n"
            "\n"
            "\tfor (const auto &warning : m_blocklist.warnings)\n"
            '\t\tWARN_LOG(SLIPPI_ONLINE, "[Blocklist] %s", warning.c_str());\n'
            "\n"
            '\tERROR_LOG(SLIPPI_ONLINE, "[Blocklist] Loaded %s: %s", path.c_str(),\n'
            "\t          SlippiBlocklist::Summary(m_blocklist).c_str());\n"
            "}\n"
            "\n"
            "bool SlippiMatchmaking::shouldSkipMatchForBlocklist()\n"
            "{\n"
            "\tif (!m_blocklist.enabled || m_blocklist.entries.empty())\n"
            "\t\treturn false;\n"
            "\n"
            "\tstd::string modeName = SlippiBlocklist::ModeName(m_searchSettings.mode);\n"
            "\tif (!SlippiBlocklist::ModeAllowsFiltering(m_blocklist, modeName))\n"
            "\t\treturn false;\n"
            "\n"
            "\tstd::string localCode = SlippiBlocklist::NormalizeCode(m_user->GetUserInfo().connectCode);\n"
            "\n"
            "\tfor (size_t i = 0; i < m_playerInfo.size(); i++)\n"
            "\t{\n"
            "\t\tconst auto &player = m_playerInfo[i];\n"
            "\n"
            "\t\t// Never act on ourselves, whichever way the server identifies us\n"
            "\t\tif ((int)i == m_localPlayerIndex ||\n"
            "\t\t    SlippiBlocklist::NormalizeCode(player.connectCode) == localCode)\n"
            "\t\t\tcontinue;\n"
            "\n"
            "\t\tconst SlippiBlocklist::Entry *hit = SlippiBlocklist::FindBlocked(m_blocklist, player.connectCode);\n"
            "\t\tif (!hit)\n"
            "\t\t\tcontinue;\n"
            "\n"
            "\t\tm_blocklistSkips++;\n"
            '\t\tERROR_LOG(SLIPPI_ONLINE, "[Blocklist] Skipping %s match: %s (%s) is blocked%s%s (skip #%d this search)",\n'
            "\t\t          modeName.c_str(), player.connectCode.c_str(), player.displayName.c_str(),\n"
            '\t\t          hit->note.empty() ? "" : " - ", hit->note.c_str(), m_blocklistSkips);\n'
            '\t\tOSD::AddMessage(StringFromFormat("Blocklist: skipped %s, searching again", player.connectCode.c_str()),\n'
            "\t\t                OSD::Duration::VERY_LONG, OSD::Color::YELLOW);\n"
            "\t\treturn true;\n"
            "\t}\n"
            "\n"
            "\treturn false;\n"
            "}\n"
            "\n"
            "int SlippiMatchmaking::LocalPlayerIndex()\n",
        ),
    ],
}


# --------------------------------------------------------------------------- #
# Mainline (2-space indent, snake_case members, fmt-style logging)
# --------------------------------------------------------------------------- #
MAINLINE = {
    "SlippiMatchmaking.h": [
        (
            "#include <nlohmann/json.hpp>\nusing json = nlohmann::json;\n",
            "#include <nlohmann/json.hpp>\n"
            "using json = nlohmann::json;\n"
            "\n"
            '#include "Core/Slippi/SlippiBlocklist.h"\n',
        ),
        (
            "  bool m_is_host;\n",
            "  bool m_is_host;\n"
            "\n"
            "  // Local connect-code blocklist (User/Slippi/blocklist.json). Reloaded on every search.\n"
            "  SlippiBlocklist::Config m_blocklist;\n"
            "  int m_blocklist_skips = 0;\n",
        ),
        (
            "  void handleConnecting();\n};",
            "  void handleConnecting();\n"
            "\n"
            "  void loadBlocklist();\n"
            "  bool shouldSkipMatchForBlocklist();\n"
            "};",
        ),
    ],
    "SlippiMatchmaking.cpp": [
        (
            '#include "Common/StringUtil.h"\n',
            '#include "Common/StringUtil.h"\n'
            '#include "Common/CommonPaths.h"\n'
            '#include "Common/FileUtil.h"\n'
            '#include "Core/Slippi/SlippiBlocklist.h"\n'
            '#include "VideoCommon/OnScreenDisplay.h"\n'
            "#include <fmt/format.h>\n",
        ),
        (
            "void SlippiMatchmaking::MatchmakeThread()\n{\n",
            "void SlippiMatchmaking::MatchmakeThread()\n{\n"
            "  // Pick up edits to blocklist.json without restarting Dolphin\n"
            "  loadBlocklist();\n"
            "\n",
        ),
        (
            '  m_is_host = get_resp.value("isHost", false);\n',
            '  m_is_host = get_resp.value("isHost", false);\n'
            "\n"
            "  // Local blocklist: if someone in this assignment is blocked, throw it away and ask\n"
            "  // for a fresh ticket instead of connecting to them. This is the same path the\n"
            "  // client already takes when a peer connection fails (see handleConnecting).\n"
            "  if (shouldSkipMatchForBlocklist())\n"
            "  {\n"
            "    terminateMmConnection();\n"
            "    m_remote_ips.clear();\n"
            "    m_player_info.clear();\n"
            "\n"
            "    int delay_ms = SlippiBlocklist::RequeueDelayForSkip(m_blocklist, m_blocklist_skips);\n"
            "    if (delay_ms > 0)\n"
            "      Common::SleepCurrentThread(delay_ms);\n"
            "\n"
            "    // Matchmaking may have been cancelled while we were waiting\n"
            "    if (m_state != ProcessState::MATCHMAKING)\n"
            "      return;\n"
            "\n"
            "    m_state = ProcessState::INITIALIZING;\n"
            "    return;\n"
            "  }\n",
        ),
        (
            "int SlippiMatchmaking::LocalPlayerIndex()\n",
            "void SlippiMatchmaking::loadBlocklist()\n"
            "{\n"
            "  m_blocklist = SlippiBlocklist::Config();\n"
            "  m_blocklist_skips = 0;\n"
            "\n"
            "  std::string path = File::GetUserPath(D_SLIPPI_IDX) + SlippiBlocklist::FILE_NAME;\n"
            "\n"
            "  if (!File::Exists(path))\n"
            "  {\n"
            '    INFO_LOG_FMT(SLIPPI_ONLINE, "[Blocklist] No {}, nothing will be filtered", path);\n'
            "    return;\n"
            "  }\n"
            "\n"
            "  std::string contents;\n"
            "  if (!File::ReadFileToString(path, contents))\n"
            "  {\n"
            '    ERROR_LOG_FMT(SLIPPI_ONLINE, "[Blocklist] Could not read {}", path);\n'
            "    return;\n"
            "  }\n"
            "\n"
            "  std::string err;\n"
            "  m_blocklist = SlippiBlocklist::Parse(contents, &err);\n"
            "  if (!err.empty())\n"
            "  {\n"
            '    ERROR_LOG_FMT(SLIPPI_ONLINE, "[Blocklist] Failed to parse {}: {}", path, err);\n'
            '    OSD::AddMessage("Blocklist: " + err, OSD::Duration::VERY_LONG, OSD::Color::RED);\n'
            "    return;\n"
            "  }\n"
            "\n"
            "  for (const auto& warning : m_blocklist.warnings)\n"
            '    WARN_LOG_FMT(SLIPPI_ONLINE, "[Blocklist] {}", warning);\n'
            "\n"
            '  ERROR_LOG_FMT(SLIPPI_ONLINE, "[Blocklist] Loaded {}: {}", path,\n'
            "                SlippiBlocklist::Summary(m_blocklist));\n"
            "}\n"
            "\n"
            "bool SlippiMatchmaking::shouldSkipMatchForBlocklist()\n"
            "{\n"
            "  if (!m_blocklist.enabled || m_blocklist.entries.empty())\n"
            "    return false;\n"
            "\n"
            "  std::string mode_name = SlippiBlocklist::ModeName(m_search_settings.mode);\n"
            "  if (!SlippiBlocklist::ModeAllowsFiltering(m_blocklist, mode_name))\n"
            "    return false;\n"
            "\n"
            "  std::string local_code =\n"
            "      SlippiBlocklist::NormalizeCode(m_user->GetUserInfo().connect_code);\n"
            "\n"
            "  for (size_t i = 0; i < m_player_info.size(); i++)\n"
            "  {\n"
            "    const auto& player = m_player_info[i];\n"
            "\n"
            "    // Never act on ourselves, whichever way the server identifies us\n"
            "    if ((int)i == m_local_player_idx ||\n"
            "        SlippiBlocklist::NormalizeCode(player.connect_code) == local_code)\n"
            "      continue;\n"
            "\n"
            "    const SlippiBlocklist::Entry* hit =\n"
            "        SlippiBlocklist::FindBlocked(m_blocklist, player.connect_code);\n"
            "    if (!hit)\n"
            "      continue;\n"
            "\n"
            "    m_blocklist_skips++;\n"
            "    ERROR_LOG_FMT(SLIPPI_ONLINE,\n"
            '                  "[Blocklist] Skipping {} match: {} ({}) is blocked{}{} (skip #{} this search)",\n'
            "                  mode_name, player.connect_code, player.display_name,\n"
            '                  hit->note.empty() ? "" : " - ", hit->note, m_blocklist_skips);\n'
            '    OSD::AddMessage(fmt::format("Blocklist: skipped {}, searching again", player.connect_code),\n'
            "                    OSD::Duration::VERY_LONG, OSD::Color::YELLOW);\n"
            "    return true;\n"
            "  }\n"
            "\n"
            "  return false;\n"
            "}\n"
            "\n"
            "int SlippiMatchmaking::LocalPlayerIndex()\n",
        ),
    ],
}

FORKS = {"ishiiruka": ISHIIRUKA, "mainline": MAINLINE}


def read_lf(path: Path) -> tuple[str, bool]:
    raw = path.read_bytes().decode("utf-8")
    crlf = "\r\n" in raw
    return raw.replace("\r\n", "\n"), crlf


def write(path: Path, text: str, crlf: bool) -> None:
    if crlf:
        text = text.replace("\n", "\r\n")
    path.write_bytes(text.encode("utf-8"))


def patch_file(path: Path, edits, check: bool) -> bool:
    text, crlf = read_lf(path)
    if MARKER in text:
        print(f"  {path.name}: already patched, skipping")
        return True
    for anchor, replacement in edits:
        n = text.count(anchor)
        if n != 1:
            print(f"  {path.name}: anchor found {n} times (need exactly 1):\n    {anchor!r}")
            return False
        text = text.replace(anchor, replacement, 1)
    if not check:
        write(path, text, crlf)
    print(f"  {path.name}: {len(edits)} edit(s) {'would apply' if check else 'applied'}")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fork", choices=sorted(FORKS), required=True)
    ap.add_argument("--repo", required=True, help="path to the Dolphin checkout")
    ap.add_argument("--check", action="store_true", help="dry run: verify anchors, write nothing")
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    slippi_dir = repo / SLIPPI_DIR
    if not slippi_dir.is_dir():
        print(f"error: {slippi_dir} does not exist; is --repo a Slippi Dolphin checkout?")
        return 2
    if not HEADER_SRC.is_file():
        print(f"error: {HEADER_SRC} missing")
        return 2

    print(f"Patching {args.fork} checkout at {repo}")
    ok = True
    for name, edits in FORKS[args.fork].items():
        ok = patch_file(slippi_dir / name, edits, args.check) and ok

    header_dst = slippi_dir / HEADER_SRC.name
    if ok and not args.check:
        shutil.copyfile(HEADER_SRC, header_dst)
        print(f"  {header_dst.name}: copied")
    elif ok:
        print(f"  {header_dst.name}: would copy")

    if not ok:
        print("error: one or more anchors did not match; nothing was written" if args.check else
              "error: one or more files could not be patched; check the tree before building")
        return 1
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
