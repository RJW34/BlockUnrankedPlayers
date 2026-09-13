<#
.SYNOPSIS
  Installs (or removes) the Slippi connect-code blocklist add-on into an existing Slippi Launcher setup.

.DESCRIPTION
  Double-click Install.cmd after extracting the release zip. Nothing is rebuilt
  or replaced: one file (dinput8.dll) is dropped next to Slippi Dolphin, and it
  forwards all DirectInput calls to Windows' own dinput8.dll while adding the
  blocklist. Your ISO, settings, replays and login are untouched.

  Steps:
    1. Finds every Slippi Dolphin the Launcher has downloaded
       (%APPDATA%\Slippi Launcher\netplay and netplay-beta).
    2. Copies dinput8.dll next to each one (backing up any dinput8.dll that
       was already there).
    3. Creates User\Slippi\blocklist.json if it does not exist yet.
    4. Opens the Blocklist Manager.

  -Uninstall removes dinput8.dll again (and restores a backup if one exists).
  Your blocklist.json is left alone.

.PARAMETER LauncherDir
  Slippi Launcher data folder. Default: %APPDATA%\Slippi Launcher
#>
[CmdletBinding()]
param(
    [string]$LauncherDir = (Join-Path $env:APPDATA 'Slippi Launcher'),
    [switch]$Uninstall,
    [switch]$NoManager,
    [switch]$NonInteractive
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Write-Step([string]$msg) { Write-Host "  * $msg" }
function Write-Ok([string]$msg)   { Write-Host "  OK  $msg" -ForegroundColor Green }
function Write-Warn2([string]$msg){ Write-Host "  !!  $msg" -ForegroundColor Yellow }
function Fail([string]$msg) {
    Write-Host ""
    Write-Host "  ERROR: $msg" -ForegroundColor Red
    Write-Host ""
    exit 1
}
function Get-Sha256([string]$path) { (Get-FileHash -Algorithm SHA256 -Path $path).Hash.ToLowerInvariant() }

Write-Host ""
Write-Host "Slippi connect-code blocklist $(if ($Uninstall) { 'uninstaller' } else { 'installer' })" -ForegroundColor Cyan
Write-Host "-----------------------------------------------"

$manifestPath = Join-Path $here 'manifest.json'
if (-not (Test-Path $manifestPath)) { Fail "manifest.json is missing next to install.ps1. Extract the whole zip, not just one file." }
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$dllName = $manifest.dllName
$ourDll = Join-Path $here $dllName
if (-not (Test-Path $ourDll)) { Fail "$dllName is missing from the package. Extract the whole zip." }
$ourHash = Get-Sha256 $ourDll
if ($manifest.dllSha256 -and $manifest.dllSha256.ToLowerInvariant() -ne $ourHash) {
    Fail "$dllName does not match manifest.json (corrupt download?). Get the zip again."
}

if (-not (Test-Path $LauncherDir)) {
    Fail "Slippi Launcher folder not found at `"$LauncherDir`".`n  Install Slippi Launcher (https://slippi.gg) and play online once, then run this again."
}

# Every Dolphin the Launcher manages
$targets = @()
foreach ($sub in @('netplay', 'netplay-beta')) {
    $dir = Join-Path $LauncherDir $sub
    foreach ($exe in $manifest.exeNames) {
        if (Test-Path (Join-Path $dir $exe)) { $targets += [pscustomobject]@{ dir = $dir; exe = (Join-Path $dir $exe); sub = $sub }; break }
    }
}
if ($targets.Count -eq 0) {
    Fail "No Slippi Dolphin found under `"$LauncherDir`".`n  Open the Slippi Launcher and play online once so it downloads Dolphin, then run this again."
}

# Refuse to touch a running Dolphin
foreach ($t in $targets) {
    $running = Get-Process | Where-Object { $_.Path -and ($_.Path -ieq $t.exe) }
    if ($running) { Fail "Slippi Dolphin is running ($($t.exe)). Close it and run this again." }
}

$firstUserDir = $null
foreach ($t in $targets) {
    $dst = Join-Path $t.dir $dllName
    $bak = "$dst.bak"
    $userSlippiDir = Join-Path $t.dir 'User\Slippi'
    $blocklistPath = Join-Path $userSlippiDir 'blocklist.json'
    Write-Step "$($t.sub): $($t.exe)"

    if ($Uninstall) {
        if (-not (Test-Path $dst)) { Write-Ok "not installed here"; continue }
        $h = Get-Sha256 $dst
        $known = @($ourHash) + @($manifest.previousSha256)
        if ($h -notin $known) {
            Write-Warn2 "$dst is not a file this package installed; leaving it alone."
            continue
        }
        Remove-Item -Force $dst
        Write-Ok "removed $dllName"
        if (Test-Path $bak) { Move-Item -Force $bak $dst; Write-Ok "restored previous $dllName from backup" }
        if (Test-Path $blocklistPath) { Write-Step "kept $blocklistPath" }
        continue
    }

    if (Test-Path $dst) {
        $h = Get-Sha256 $dst
        if ($h -eq $ourHash) {
            Write-Ok "$dllName already up to date"
        } else {
            if (-not (Test-Path $bak)) { Copy-Item -Force $dst $bak; Write-Warn2 "a different $dllName was here; saved it as $(Split-Path -Leaf $bak)" }
            Copy-Item -Force $ourDll $dst
            Write-Ok "updated $dllName"
        }
    } else {
        Copy-Item -Force $ourDll $dst
        Write-Ok "installed $dllName"
    }

    if (-not (Test-Path $userSlippiDir)) { New-Item -ItemType Directory -Force $userSlippiDir | Out-Null }
    if (-not (Test-Path $blocklistPath)) {
        $template = Join-Path $here 'blocklist.template.json'
        if (Test-Path $template) { Copy-Item $template $blocklistPath }
        else {
            $default = [ordered]@{ enabled = $true; modes = @('unranked'); requeueDelayMs = 1000; beepOnSkip = $true; blocked = @() } | ConvertTo-Json -Depth 4
            [System.IO.File]::WriteAllText($blocklistPath, $default + "`n", (New-Object System.Text.UTF8Encoding $false))
        }
        Write-Ok "created $blocklistPath"
    } else {
        Write-Ok "keeping existing $blocklistPath"
    }
    if (-not $firstUserDir) { $firstUserDir = $userSlippiDir }
}

Write-Host ""
if ($Uninstall) {
    Write-Host "  Done. Slippi is back to stock." -ForegroundColor Green
    exit 0
}

Write-Host "  Done. Blocked players are skipped automatically in Unranked." -ForegroundColor Green
Write-Host "  Manage the list any time with `"Blocklist Manager.cmd`"."
Write-Host "  Check it is working: after your next online session, open"
Write-Host "    $firstUserDir\blocklist.log"
Write-Host "  and look for 'hooks: WSARecvFrom=ok WSASendTo=ok' at the top."
Write-Host "  If Slippi updates and the skipping stops, just run Install.cmd again."
Write-Host ""

if (-not $NoManager -and -not $NonInteractive) {
    $manager = Join-Path $here 'BlocklistManager.ps1'
    if (Test-Path $manager) {
        Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', "`"$manager`"", '-LauncherDir', "`"$LauncherDir`"")
    }
}
exit 0
