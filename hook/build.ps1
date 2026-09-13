<#
.SYNOPSIS
  Builds hook/dllmain.cpp into dist/dinput8.dll with MinGW-w64 (no Visual Studio needed).

.DESCRIPTION
  Looks for x86_64 g++ in PATH, then in the usual MSYS2 / WinLibs locations.
  nlohmann/json.hpp is fetched into build/include once if it is not there
  (it is the same single header Dolphin bundles).

  -Test   also builds and runs tests/test_blocklist.cpp, tests/test_hook_core.cpp
          and the in-process loopback test against the freshly built DLL.
#>
[CmdletBinding()]
param(
    [switch]$Test,
    [string]$Gxx
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$build = Join-Path $root 'build'
$inc = Join-Path $build 'include'
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $build, $inc, (Join-Path $inc 'nlohmann'), $dist | Out-Null

function Find-Gxx {
    if ($Gxx) { return $Gxx }
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in @('C:\msys64\mingw64\bin\g++.exe', 'C:\msys64\ucrt64\bin\g++.exe', 'C:\mingw64\bin\g++.exe', "$env:LOCALAPPDATA\Programs\WinLibs\mingw64\bin\g++.exe")) {
        if (Test-Path $p) { return $p }
    }
    throw "g++ not found. Install MSYS2 (https://www.msys2.org) and run: pacman -S mingw-w64-x86_64-gcc  (or pass -Gxx path\to\g++.exe)"
}
$gxx = Find-Gxx
$gxxDir = Split-Path -Parent $gxx
$env:PATH = "$gxxDir;$env:PATH"
Write-Host "Using $gxx"

$json = Join-Path $inc 'nlohmann\json.hpp'
if (-not (Test-Path $json)) {
    Write-Host 'Fetching nlohmann/json.hpp (same version Slippi bundles)...'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://raw.githubusercontent.com/project-slippi/Ishiiruka/slippi/Externals/nlohmann/json.hpp' -OutFile $json
}
Copy-Item -Force $json (Join-Path $inc 'json.hpp')

$common = @('-std=c++17', '-O2', '-Wall', '-Wextra', "-I$root\dolphin", "-I$root\hook", "-I$inc")
$dll = Join-Path $dist 'dinput8.dll'
Write-Host "Building $dll"
& $gxx @common -shared -static -static-libgcc -static-libstdc++ -o $dll "$root\hook\dllmain.cpp" -lws2_32
if ($LASTEXITCODE -ne 0) { throw "DLL build failed" }
$hash = (Get-FileHash -Algorithm SHA256 $dll).Hash.ToLowerInvariant()
Write-Host "OK  $dll  sha256=$hash  $((Get-Item $dll).Length) bytes"

$manifest = Join-Path $dist 'manifest.json'
if (Test-Path $manifest) {
    $m = Get-Content $manifest -Raw | ConvertFrom-Json
    $ver = (Select-String -Path "$root\hook\dllmain.cpp" -Pattern '#define HOOK_VERSION "([^"]+)"').Matches[0].Groups[1].Value
    $m | Add-Member -NotePropertyName dllSha256 -NotePropertyValue $hash -Force
    $m | Add-Member -NotePropertyName hookVersion -NotePropertyValue $ver -Force
    [System.IO.File]::WriteAllText($manifest, (($m | ConvertTo-Json -Depth 4) + "`n"), (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Updated $manifest"
}

if ($Test) {
    $t1 = Join-Path $build 'test_blocklist.exe'
    $t2 = Join-Path $build 'test_hook_core.exe'
    $t3 = Join-Path $build 'test_hook_integration.exe'
    & $gxx @common -o $t1 "$root\tests\test_blocklist.cpp";      if ($LASTEXITCODE) { throw 'test_blocklist build failed' }
    & $gxx @common -o $t2 "$root\tests\test_hook_core.cpp";      if ($LASTEXITCODE) { throw 'test_hook_core build failed' }
    & $gxx @common -static -static-libgcc -static-libstdc++ -o $t3 "$root\tests\test_hook_integration.cpp" -lws2_32 -lole32
    if ($LASTEXITCODE) { throw 'test_hook_integration build failed' }
    Copy-Item -Force $dll (Join-Path $build 'dinput8.dll')
    foreach ($t in @($t1, $t2, $t3)) {
        Write-Host "--- $(Split-Path -Leaf $t)"
        & $t
        if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $t) FAILED" }
    }
    Write-Host 'All tests passed.' -ForegroundColor Green
}
