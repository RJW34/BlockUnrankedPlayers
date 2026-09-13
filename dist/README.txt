Slippi connect-code blocklist  (skip specific players in Unranked)
====================================================================

INSTALL
  1. You need Slippi Launcher installed and to have played online at least once.
  2. Close Slippi Dolphin if it is open.
  3. Double-click  Install.cmd
     (Windows may ask "Windows protected your PC" -> More info -> Run anyway.
      Nothing is replaced: one file, dinput8.dll, is added next to Slippi Dolphin.)
  4. The Blocklist Manager window opens. Type a connect code like ABCD#123 and
     press Add, or click "Recent opponents..." to pick from people you just played.

USE
  Queue Unranked like normal. If the server pairs you with someone on your list,
  Dolphin never connects to them: you hear a short beep, the search continues by
  itself a few seconds later, and the other person just sees a longer search.

  Ranked and Direct are never filtered.
  Edit the list any time with  "Blocklist Manager.cmd"  - changes apply the next
  time you press Search.

CHECK IT IS WORKING
  After playing online once, open
    %APPDATA%\Slippi Launcher\netplay\User\Slippi\blocklist.log
  The first lines should say  "hooks: WSARecvFrom=ok WSASendTo=ok"  and every
  match shows up as  "match found (unranked): ...". Skips are logged as "SKIP: ...".

REMOVE
  Double-click  Uninstall.cmd   (deletes dinput8.dll; your list file is kept).

WHEN SLIPPI UPDATES
  Slippi Launcher may replace the Dolphin folder when it updates. If blocked
  players stop being skipped, just run Install.cmd again.
