<#
.SYNOPSIS
  Runs every test in this repo: C++ unit tests, the in-process DLL loopback test,
  the installer against a throwaway copy of a Slippi Launcher folder, and the
  Blocklist Manager command-line interface (including .slp replay parsing).

  Needs MinGW g++ for the C++ parts (see hook/build.ps1); the PowerShell parts
  run anywhere.
#>
[CmdletBinding()]
param(
    [switch]$SkipCpp
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$failures = 0
function Check([bool]$cond, [string]$what) {
    if ($cond) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $script:failures++ }
}

# --------------------------------------------------------------------------- #
if (-not $SkipCpp) {
    Write-Host "== C++ build + unit + loopback tests" -ForegroundColor Cyan
    & (Join-Path $root 'hook\build.ps1') -Test
    if ($LASTEXITCODE -ne 0) { throw 'C++ tests failed' }
}

# --------------------------------------------------------------------------- #
Write-Host "== installer against a fake Slippi Launcher folder" -ForegroundColor Cyan
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("slippi-blocklist-test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$fakeLauncher = Join-Path $tmp 'Slippi Launcher'
$netplay = Join-Path $fakeLauncher 'netplay'
New-Item -ItemType Directory -Force (Join-Path $netplay 'User\Slippi') | Out-Null
Set-Content -Path (Join-Path $netplay 'Slippi Dolphin.exe') -Value 'not really an exe'
Set-Content -Path (Join-Path $netplay 'User\Slippi\user.json') -Value '{"connectCode":"ABS#0","displayName":"me"}'
Set-Content -Path (Join-Path $fakeLauncher 'Settings') -Value '{"settings":{"useNetplayBeta":false}}'
$dist = Join-Path $root 'dist'
$installer = Join-Path $dist 'install.ps1'
$dll = Join-Path $dist 'dinput8.dll'
Check (Test-Path $dll) "dist\dinput8.dll exists (build it with hook\build.ps1)"

& powershell -NoProfile -ExecutionPolicy Bypass -File $installer -LauncherDir $fakeLauncher -NonInteractive | Out-Null
Check ($LASTEXITCODE -eq 0) "install exits 0"
Check (Test-Path (Join-Path $netplay 'dinput8.dll')) "dinput8.dll copied next to Slippi Dolphin.exe"
Check ((Get-FileHash (Join-Path $netplay 'dinput8.dll')).Hash -eq (Get-FileHash $dll).Hash) "copied DLL matches package"
$bl = Join-Path $netplay 'User\Slippi\blocklist.json'
Check (Test-Path $bl) "blocklist.json created"
Check ((Get-Content $bl -Raw | ConvertFrom-Json).modes -contains 'unranked') "blocklist.json defaults to unranked"

# second run is a no-op and keeps an edited blocklist
Set-Content -Path $bl -Value '{"enabled":true,"modes":["unranked"],"blocked":["KEEP#1"]}'
& powershell -NoProfile -ExecutionPolicy Bypass -File $installer -LauncherDir $fakeLauncher -NonInteractive | Out-Null
Check ($LASTEXITCODE -eq 0) "re-install exits 0"
Check ((Get-Content $bl -Raw) -match 'KEEP#1') "re-install keeps existing blocklist.json"

# a foreign dinput8.dll gets backed up, then restored on uninstall
Set-Content -Path (Join-Path $netplay 'dinput8.dll') -Value 'someone elses mod'
& powershell -NoProfile -ExecutionPolicy Bypass -File $installer -LauncherDir $fakeLauncher -NonInteractive | Out-Null
Check (Test-Path (Join-Path $netplay 'dinput8.dll.bak')) "foreign dinput8.dll backed up"
& powershell -NoProfile -ExecutionPolicy Bypass -File $installer -LauncherDir $fakeLauncher -Uninstall -NonInteractive | Out-Null
Check ($LASTEXITCODE -eq 0) "uninstall exits 0"
Check ((Get-Content (Join-Path $netplay 'dinput8.dll') -Raw) -match 'someone elses mod') "uninstall restored the backup"
Check (Test-Path $bl) "uninstall keeps blocklist.json"
Remove-Item -Force (Join-Path $netplay 'dinput8.dll')
& powershell -NoProfile -ExecutionPolicy Bypass -File $installer -LauncherDir $fakeLauncher -Uninstall -NonInteractive | Out-Null
Check ($LASTEXITCODE -eq 0) "uninstall when not installed exits 0"

# missing launcher folder is a clean error
& powershell -NoProfile -ExecutionPolicy Bypass -File $installer -LauncherDir (Join-Path $tmp 'nope') -NonInteractive | Out-Null
Check ($LASTEXITCODE -eq 1) "install with no launcher exits 1"

# --------------------------------------------------------------------------- #
Write-Host "== Blocklist Manager CLI" -ForegroundColor Cyan
$mgr = Join-Path $dist 'BlocklistManager.ps1'
$blPath = Join-Path $tmp 'blocklist.json'
function Mgr {
    param([string[]]$a)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'   # a non-zero exit / stderr line from the child is a result, not a crash
    try { & powershell -NoProfile -ExecutionPolicy Bypass -File $mgr -LauncherDir $fakeLauncher -BlocklistPath $blPath @a 2>&1 | Out-String }
    finally { $ErrorActionPreference = $prev }
}

$out = Mgr @('-Add', 'abcd#123', '-Note', 'test note')
Check ($out -match 'Added ABCD#123') "add normalizes case"
$j = Get-Content $blPath -Raw | ConvertFrom-Json
Check ($j.blocked.Count -eq 1 -and $j.blocked[0].code -eq 'ABCD#123' -and $j.blocked[0].note -eq 'test note') "file has the entry"
Check ($j.enabled -eq $true -and $j.modes -contains 'unranked' -and $j.beepOnSkip -eq $true) "file has defaults"
$out = Mgr @('-Add', 'ABCD#123')
Check ($out -match 'already blocked') "duplicate add is reported"
# full-width number sign (as shown in-game) is normalized when read from the file
$fw = [string][char]0xFF03
[System.IO.File]::WriteAllText($blPath, "{`"blocked`":[`"abcd#123`",{`"code`":`"wxyz${fw}9`",`"note`":`"fw`"}]}", (New-Object System.Text.UTF8Encoding $false))
$out = Mgr @('-List')
Check ($out -match 'WXYZ#9') "full-width # is normalized"
$out = Mgr @('-Add', 'ABCD#123', '-Note', 'test note')   # re-add note lost by the rewrite above
Check ($out -match 'already blocked') "note update on existing entry"
$out = Mgr @('-Add', 'not a code')
Check ($out -match 'does not look like a connect code') "invalid code rejected"
$out = Mgr @('-Add', 'ABS#0')
Check ($out -match 'your own connect code') "own code rejected"
$out = Mgr @('-Remove', 'wxyz#9')
Check ($out -match 'Removed WXYZ#9') "remove works"
$out = Mgr @('-Disable')
Check ((Get-Content $blPath -Raw | ConvertFrom-Json).enabled -eq $false) "disable works"
$out = Mgr @('-Enable', '-Mode', 'unranked,teams')
$j = Get-Content $blPath -Raw | ConvertFrom-Json
Check ($j.enabled -eq $true -and $j.modes.Count -eq 2 -and $j.modes -contains 'teams') "enable + modes works"
$out = Mgr @('-Mode', 'ranked')
Check ($out -match 'Unknown mode') "ranked mode rejected"
$out = Mgr @('-List')
Check ($out -match 'ABCD#123' -and $out -match 'test note') "list shows entries"

# the C++ parser accepts what PowerShell writes (BOM-free, arrays intact)
$bytes = [System.IO.File]::ReadAllBytes($blPath)
Check (-not ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB)) "file written without BOM"
Check ((Get-Content $blPath -Raw) -match '"modes":\s*\[') "modes serialized as an array"

# replay parsing against real replays if this machine has any
$replayDir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Slippi'
if (Test-Path $replayDir) {
    $out = Mgr @('-Recent', '-Count', '5', '-ReplayDir', $replayDir)
    Check ($out -match '#\d') "recent opponents parsed from real replays"
    Write-Host ($out.Trim() -split "`n" | Select-Object -First 8 | ForEach-Object { "       $_" })
} else {
    Write-Host "  skip replay parsing (no $replayDir)"
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
Write-Host ""
if ($failures) { Write-Host "$failures check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "All PowerShell checks passed." -ForegroundColor Green
exit 0
