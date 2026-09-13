<#
.SYNOPSIS
  Slippi Blocklist Manager: edit User\Slippi\blocklist.json from a small window or the command line.

.DESCRIPTION
  No parameters  -> opens the window.
  -List          -> print the blocklist
  -Add ABCD#123 [-Note "why"]
  -Remove ABCD#123
  -Recent [-Count 20]   -> list the people you played most recently (from your .slp replays)
  -Enable / -Disable
  -Mode unranked,teams  -> set which modes are filtered (unranked, teams, party)

  Works with Windows PowerShell 5.1 (built into Windows) and PowerShell 7.
  The file lives next to user.json:  %APPDATA%\Slippi Launcher\netplay\User\Slippi\blocklist.json
#>
[CmdletBinding()]
param(
    [switch]$List,
    [string]$Add,
    [string]$Note = '',
    [string]$Remove,
    [switch]$Recent,
    [int]$Count = 20,
    [switch]$Enable,
    [switch]$Disable,
    [string[]]$Mode,
    [string]$BlocklistPath,
    [string]$ReplayDir,
    [string]$LauncherDir = (Join-Path $env:APPDATA 'Slippi Launcher')
)

$ErrorActionPreference = 'Stop'
$script:ValidModes = @('unranked', 'teams', 'party')

# --------------------------------------------------------------------------- #
# Paths
# --------------------------------------------------------------------------- #
function Get-SlippiUserDir([string]$LauncherDir) {
    $sub = 'netplay'
    $settings = Join-Path $LauncherDir 'Settings'
    if (Test-Path $settings) {
        try {
            $s = Get-Content $settings -Raw | ConvertFrom-Json
            if ($s.settings.useNetplayBeta) { $sub = 'netplay-beta' }
        } catch { }
    }
    return (Join-Path $LauncherDir "$sub\User\Slippi")
}

function Get-OwnConnectCode([string]$userSlippiDir) {
    $userJson = Join-Path $userSlippiDir 'user.json'
    if (-not (Test-Path $userJson)) { return '' }
    try { return (ConvertTo-NormalizedCode ([string](Get-Content $userJson -Raw | ConvertFrom-Json).connectCode)) } catch { return '' }
}

function Get-ReplayDir([string]$userSlippiDir) {
    $ini = Join-Path (Split-Path $userSlippiDir -Parent) 'Config\Dolphin.ini'
    if (Test-Path $ini) {
        $m = Select-String -Path $ini -Pattern '^\s*SlippiReplayDir\s*=\s*(.+?)\s*$' | Select-Object -First 1
        if ($m) {
            $d = $m.Matches[0].Groups[1].Value
            if (Test-Path $d) { return $d }
        }
    }
    return (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Slippi')
}

# --------------------------------------------------------------------------- #
# Codes and the blocklist file
# --------------------------------------------------------------------------- #
function ConvertTo-NormalizedCode([string]$code) {
    if ([string]::IsNullOrWhiteSpace($code)) { return '' }
    $c = $code.Replace([string][char]0xFF03, '#')   # full-width number sign, as rendered in-game
    $c = ($c -replace '\s', '').ToUpperInvariant()
    return $c
}

function Test-ConnectCode([string]$code) { return ($code -match '^[A-Z0-9]{1,8}#[0-9]{1,4}$') }

function New-Blocklist {
    return [pscustomobject]@{ enabled = $true; modes = @('unranked'); requeueDelayMs = 1000; beepOnSkip = $true; blocked = @() }
}

function Read-Blocklist([string]$path) {
    $bl = New-Blocklist
    if (-not (Test-Path $path)) { return $bl }
    $raw = [System.IO.File]::ReadAllText($path)
    if ([string]::IsNullOrWhiteSpace($raw)) { return $bl }
    $j = $raw | ConvertFrom-Json
    $list = $null
    if ($j -is [array]) {
        $list = $j
    } else {
        if ($null -ne $j.enabled) { $bl.enabled = [bool]$j.enabled }
        if ($null -ne $j.modes) {
            $bl.modes = @(@($j.modes) | ForEach-Object { "$_".ToLowerInvariant() } | Where-Object { $_ -in $script:ValidModes } | Select-Object -Unique)
        }
        if ($null -ne $j.requeueDelayMs) { $bl.requeueDelayMs = [Math]::Max(0, [Math]::Min(30000, [int]$j.requeueDelayMs)) }
        if ($null -ne $j.beepOnSkip) { $bl.beepOnSkip = [bool]$j.beepOnSkip }
        $list = $j.blocked
    }
    $entries = @()
    foreach ($e in @($list)) {
        if ($null -eq $e) { continue }
        if ($e -is [string]) { $code = ConvertTo-NormalizedCode $e; $n = '' }
        else { $code = ConvertTo-NormalizedCode ([string]$e.code); $n = [string]$e.note }
        if (-not $code) { continue }
        if (@($entries | Where-Object { $_.code -eq $code }).Count -gt 0) { continue }
        $entries += [pscustomobject]@{ code = $code; note = $n }
    }
    $bl.blocked = $entries
    return $bl
}

function Write-Blocklist([string]$path, $bl) {
    $obj = [ordered]@{
        enabled        = [bool]$bl.enabled
        modes          = @($bl.modes)
        requeueDelayMs = [int]$bl.requeueDelayMs
        beepOnSkip     = [bool]$bl.beepOnSkip
        blocked        = @(@($bl.blocked) | ForEach-Object { [ordered]@{ code = $_.code; note = [string]$_.note } })
    }
    $json = ($obj | ConvertTo-Json -Depth 5)
    $dir = Split-Path $path -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    [System.IO.File]::WriteAllText($path, $json + "`n", (New-Object System.Text.UTF8Encoding $false))
}

function Add-BlockedCode($bl, [string]$rawCode, [string]$note) {
    $code = ConvertTo-NormalizedCode $rawCode
    if (-not $code) { throw "Connect code is empty." }
    if (-not (Test-ConnectCode $code)) { throw "'$code' does not look like a connect code (expected something like ABCD#123)." }
    $existing = @($bl.blocked | Where-Object { $_.code -eq $code })
    if ($existing.Count -gt 0) {
        if ($note) { $existing[0].note = $note }
        return $false
    }
    $bl.blocked = @($bl.blocked) + [pscustomobject]@{ code = $code; note = $note }
    return $true
}

function Remove-BlockedCode($bl, [string]$rawCode) {
    $code = ConvertTo-NormalizedCode $rawCode
    $before = @($bl.blocked).Count
    $bl.blocked = @($bl.blocked | Where-Object { $_.code -ne $code })
    return (@($bl.blocked).Count -lt $before)
}

# --------------------------------------------------------------------------- #
# Replay (.slp) parsing: read the Game Start event, which carries every
# player's connect code and display name (Slippi replay spec, 0x36 event).
# --------------------------------------------------------------------------- #
function Convert-SjisField([byte[]]$bytes, $enc) {
    $len = [Array]::IndexOf($bytes, [byte]0)
    if ($len -lt 0) { $len = $bytes.Length }
    if ($len -eq 0) { return '' }
    $slice = $bytes[0..($len - 1)]
    if ($enc) { return $enc.GetString($slice) }
    return [System.Text.Encoding]::ASCII.GetString($slice)
}

function Read-ReplayPlayers([string]$path) {
    $buf = New-Object byte[] 4096
    $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try { $n = $fs.Read($buf, 0, $buf.Length) } finally { $fs.Dispose() }
    # "{U\x03raw[$U#l" + 4-byte length = 15 bytes, then the Event Payloads (0x35) event
    $raw = 15
    if ($n -lt 0x300 -or $buf[$raw] -ne 0x35) { return @() }
    $gs = $raw + 1 + [int]$buf[$raw + 1]
    if (($gs + 0x249) -gt $n -or $buf[$gs] -ne 0x36) { return @() }
    $ver = ([int]$buf[$gs + 1] * 65536) + ([int]$buf[$gs + 2] * 256) + [int]$buf[$gs + 3]
    if ($ver -lt 0x030900) { return @() }   # connect codes were added in replay format 3.9.0
    $enc = $null
    try { $enc = [System.Text.Encoding]::GetEncoding(932) } catch { }
    $players = @()
    for ($i = 0; $i -lt 4; $i++) {
        $ptype = $buf[$gs + 0x65 + 0x24 * $i + 1]      # 0 human, 1 cpu, 2 demo, 3 empty
        if ($ptype -ne 0) { continue }
        $c0 = $gs + 0x221 + 0xA * $i
        $code = ConvertTo-NormalizedCode (Convert-SjisField $buf[$c0..($c0 + 9)] $enc)
        if (-not $code) { continue }
        $n0 = $gs + 0x1A5 + 0x1F * $i
        $name = Convert-SjisField $buf[$n0..($n0 + 30)] $enc
        $players += [pscustomobject]@{ port = $i + 1; code = $code; name = $name }
    }
    return $players
}

function Get-RecentOpponents([string]$replayDir, [int]$count, [string]$ownCode) {
    if (-not (Test-Path $replayDir)) { return @() }
    $files = Get-ChildItem -Path $replayDir -Filter '*.slp' -File -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First ([Math]::Max($count * 3, 30))
    $seen = @{}
    $result = @()
    foreach ($f in $files) {
        $players = @()
        try { $players = Read-ReplayPlayers $f.FullName } catch { continue }
        if ($players.Count -lt 2) { continue }
        if ($ownCode -and -not ($players | Where-Object { $_.code -eq $ownCode })) { continue }  # spectated / not my game
        foreach ($p in $players) {
            if ($ownCode -and $p.code -eq $ownCode) { continue }
            if ($seen.ContainsKey($p.code)) { $seen[$p.code].games++; continue }
            $entry = [pscustomobject]@{ code = $p.code; name = $p.name; lastPlayed = $f.LastWriteTime; games = 1; file = $f.Name }
            $seen[$p.code] = $entry
            $result += $entry
            if ($result.Count -ge $count) { break }
        }
        if ($result.Count -ge $count) { break }
    }
    return $result
}

# --------------------------------------------------------------------------- #
# GUI
# --------------------------------------------------------------------------- #
function Show-Gui([string]$path, [string]$replayDir, [string]$ownCode) {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [System.Windows.Forms.Application]::EnableVisualStyles()

    $state = @{ bl = (Read-Blocklist $path) }

    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'Slippi Blocklist Manager'
    $form.Size = New-Object System.Drawing.Size(640, 520)
    $form.MinimumSize = New-Object System.Drawing.Size(560, 420)
    $form.StartPosition = 'CenterScreen'
    $form.Font = New-Object System.Drawing.Font('Segoe UI', 9.5)

    $pathLabel = New-Object System.Windows.Forms.Label
    $pathLabel.Text = "File: $path"
    $pathLabel.AutoEllipsis = $true
    $pathLabel.Location = New-Object System.Drawing.Point(12, 10)
    $pathLabel.Size = New-Object System.Drawing.Size(600, 20)
    $pathLabel.Anchor = 'Top,Left,Right'
    $form.Controls.Add($pathLabel)

    $enabledBox = New-Object System.Windows.Forms.CheckBox
    $enabledBox.Text = 'Blocklist enabled'
    $enabledBox.Location = New-Object System.Drawing.Point(12, 36)
    $enabledBox.AutoSize = $true
    $form.Controls.Add($enabledBox)

    $modeLabel = New-Object System.Windows.Forms.Label
    $modeLabel.Text = 'Filter in:'
    $modeLabel.Location = New-Object System.Drawing.Point(180, 38)
    $modeLabel.AutoSize = $true
    $form.Controls.Add($modeLabel)

    $modeBoxes = @{}
    $x = 245
    foreach ($m in $script:ValidModes) {
        $cb = New-Object System.Windows.Forms.CheckBox
        $cb.Text = (Get-Culture).TextInfo.ToTitleCase($m)
        $cb.Location = New-Object System.Drawing.Point($x, 36)
        $cb.AutoSize = $true
        $form.Controls.Add($cb)
        $modeBoxes[$m] = $cb
        $x += 90
    }

    $rankedNote = New-Object System.Windows.Forms.Label
    $rankedNote.Text = 'Ranked and Direct are never filtered.'
    $rankedNote.ForeColor = [System.Drawing.Color]::Gray
    $rankedNote.Location = New-Object System.Drawing.Point(12, 60)
    $rankedNote.AutoSize = $true
    $form.Controls.Add($rankedNote)

    $beepBox = New-Object System.Windows.Forms.CheckBox
    $beepBox.Text = 'Beep when someone is skipped'
    $beepBox.Location = New-Object System.Drawing.Point(300, 58)
    $beepBox.AutoSize = $true
    $form.Controls.Add($beepBox)

    $listView = New-Object System.Windows.Forms.ListView
    $listView.View = 'Details'
    $listView.FullRowSelect = $true
    $listView.GridLines = $true
    $listView.MultiSelect = $true
    $listView.Location = New-Object System.Drawing.Point(12, 86)
    $listView.Size = New-Object System.Drawing.Size(440, 330)
    $listView.Anchor = 'Top,Bottom,Left,Right'
    [void]$listView.Columns.Add('Connect code', 130)
    [void]$listView.Columns.Add('Note', 290)
    $form.Controls.Add($listView)

    $codeBox = New-Object System.Windows.Forms.TextBox
    $codeBox.Location = New-Object System.Drawing.Point(464, 86)
    $codeBox.Size = New-Object System.Drawing.Size(150, 24)
    $codeBox.Anchor = 'Top,Right'
    $codeBox.CharacterCasing = 'Upper'
    $form.Controls.Add($codeBox)
    $codeHint = New-Object System.Windows.Forms.Label
    $codeHint.Text = 'Code, e.g. ABCD#123'
    $codeHint.ForeColor = [System.Drawing.Color]::Gray
    $codeHint.Location = New-Object System.Drawing.Point(464, 112)
    $codeHint.AutoSize = $true
    $codeHint.Anchor = 'Top,Right'
    $form.Controls.Add($codeHint)

    $noteBox = New-Object System.Windows.Forms.TextBox
    $noteBox.Location = New-Object System.Drawing.Point(464, 134)
    $noteBox.Size = New-Object System.Drawing.Size(150, 24)
    $noteBox.Anchor = 'Top,Right'
    $form.Controls.Add($noteBox)
    $noteHint = New-Object System.Windows.Forms.Label
    $noteHint.Text = 'Note (optional)'
    $noteHint.ForeColor = [System.Drawing.Color]::Gray
    $noteHint.Location = New-Object System.Drawing.Point(464, 160)
    $noteHint.AutoSize = $true
    $noteHint.Anchor = 'Top,Right'
    $form.Controls.Add($noteHint)

    $addBtn = New-Object System.Windows.Forms.Button
    $addBtn.Text = 'Add'
    $addBtn.Location = New-Object System.Drawing.Point(464, 184)
    $addBtn.Size = New-Object System.Drawing.Size(150, 30)
    $addBtn.Anchor = 'Top,Right'
    $form.Controls.Add($addBtn)

    $recentBtn = New-Object System.Windows.Forms.Button
    $recentBtn.Text = 'Recent opponents...'
    $recentBtn.Location = New-Object System.Drawing.Point(464, 222)
    $recentBtn.Size = New-Object System.Drawing.Size(150, 30)
    $recentBtn.Anchor = 'Top,Right'
    $form.Controls.Add($recentBtn)

    $removeBtn = New-Object System.Windows.Forms.Button
    $removeBtn.Text = 'Remove selected'
    $removeBtn.Location = New-Object System.Drawing.Point(464, 260)
    $removeBtn.Size = New-Object System.Drawing.Size(150, 30)
    $removeBtn.Anchor = 'Top,Right'
    $form.Controls.Add($removeBtn)

    $openBtn = New-Object System.Windows.Forms.Button
    $openBtn.Text = 'Open folder'
    $openBtn.Location = New-Object System.Drawing.Point(464, 386)
    $openBtn.Size = New-Object System.Drawing.Size(150, 30)
    $openBtn.Anchor = 'Bottom,Right'
    $form.Controls.Add($openBtn)

    $status = New-Object System.Windows.Forms.Label
    $status.Location = New-Object System.Drawing.Point(12, 424)
    $status.Size = New-Object System.Drawing.Size(600, 40)
    $status.Anchor = 'Bottom,Left,Right'
    $status.ForeColor = [System.Drawing.Color]::DimGray
    $form.Controls.Add($status)

    $refresh = {
        $listView.BeginUpdate()
        $listView.Items.Clear()
        foreach ($e in @($state.bl.blocked)) {
            $item = New-Object System.Windows.Forms.ListViewItem($e.code)
            [void]$item.SubItems.Add([string]$e.note)
            [void]$listView.Items.Add($item)
        }
        $listView.EndUpdate()
        $enabledBox.Checked = [bool]$state.bl.enabled
        $beepBox.Checked = [bool]$state.bl.beepOnSkip
        foreach ($m in $script:ValidModes) { $modeBoxes[$m].Checked = ($m -in @($state.bl.modes)) }
        $n = @($state.bl.blocked).Count
        $status.Text = "$n blocked code(s). Changes are saved immediately and picked up the next time you press Search in Unranked."
    }

    $save = {
        param([string]$what)
        $state.bl.enabled = $enabledBox.Checked
        $state.bl.beepOnSkip = $beepBox.Checked
        $state.bl.modes = @($script:ValidModes | Where-Object { $modeBoxes[$_].Checked })
        try {
            Write-Blocklist $path $state.bl
            & $refresh
            if ($what) { $status.Text = "$what  |  " + $status.Text }
        } catch {
            [System.Windows.Forms.MessageBox]::Show("Could not save:`n$($_.Exception.Message)", 'Blocklist', 'OK', 'Error') | Out-Null
        }
    }

    $doAdd = {
        try {
            $added = Add-BlockedCode $state.bl $codeBox.Text $noteBox.Text
            $codeNorm = ConvertTo-NormalizedCode $codeBox.Text
            if ($ownCode -and $codeNorm -eq $ownCode) {
                [void](Remove-BlockedCode $state.bl $codeNorm)
                [System.Windows.Forms.MessageBox]::Show("That is your own connect code.", 'Blocklist', 'OK', 'Information') | Out-Null
                return
            }
            & $save $(if ($added) { "Added $codeNorm." } else { "$codeNorm was already listed; note updated." })
            $codeBox.Clear(); $noteBox.Clear(); $codeBox.Focus()
        } catch {
            [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Blocklist', 'OK', 'Warning') | Out-Null
        }
    }
    $addBtn.Add_Click($doAdd)
    $codeBox.Add_KeyDown({ param($s, $e) if ($e.KeyCode -eq 'Return') { $e.SuppressKeyPress = $true; & $doAdd } })
    $noteBox.Add_KeyDown({ param($s, $e) if ($e.KeyCode -eq 'Return') { $e.SuppressKeyPress = $true; & $doAdd } })

    $removeBtn.Add_Click({
        $sel = @($listView.SelectedItems | ForEach-Object { $_.Text })
        if ($sel.Count -eq 0) { $status.Text = 'Select one or more codes in the list first.'; return }
        foreach ($c in $sel) { [void](Remove-BlockedCode $state.bl $c) }
        & $save "Removed $($sel -join ', ')."
    })
    $listView.Add_KeyDown({ param($s, $e) if ($e.KeyCode -eq 'Delete') { $removeBtn.PerformClick() } })

    $enabledBox.Add_Click({ & $save $(if ($enabledBox.Checked) { 'Blocklist enabled.' } else { 'Blocklist disabled (nothing will be skipped).' }) })
    $beepBox.Add_Click({ & $save 'Saved.' })
    foreach ($m in $script:ValidModes) { $modeBoxes[$m].Add_Click({ & $save 'Modes updated.' }) }

    $openBtn.Add_Click({ Start-Process explorer.exe "/select,`"$path`"" })

    $recentBtn.Add_Click({
        $form.Cursor = 'WaitCursor'
        $status.Text = "Reading replays in $replayDir ..."
        [System.Windows.Forms.Application]::DoEvents()
        try { $recent = @(Get-RecentOpponents $replayDir 30 $ownCode) } catch { $recent = @() }
        $form.Cursor = 'Default'
        if ($recent.Count -eq 0) {
            [System.Windows.Forms.MessageBox]::Show("No online replays with connect codes found in:`n$replayDir", 'Recent opponents', 'OK', 'Information') | Out-Null
            & $refresh
            return
        }
        $dlg = New-Object System.Windows.Forms.Form
        $dlg.Text = 'Recent opponents (newest first)'
        $dlg.Size = New-Object System.Drawing.Size(560, 420)
        $dlg.StartPosition = 'CenterParent'
        $dlg.Font = $form.Font
        $lv = New-Object System.Windows.Forms.ListView
        $lv.View = 'Details'; $lv.FullRowSelect = $true; $lv.GridLines = $true; $lv.MultiSelect = $true
        $lv.Location = New-Object System.Drawing.Point(12, 12)
        $lv.Size = New-Object System.Drawing.Size(520, 300)
        $lv.Anchor = 'Top,Bottom,Left,Right'
        [void]$lv.Columns.Add('Connect code', 110)
        [void]$lv.Columns.Add('Name', 150)
        [void]$lv.Columns.Add('Last played', 130)
        [void]$lv.Columns.Add('Games', 55)
        [void]$lv.Columns.Add('Status', 65)
        foreach ($r in $recent) {
            $it = New-Object System.Windows.Forms.ListViewItem($r.code)
            [void]$it.SubItems.Add([string]$r.name)
            [void]$it.SubItems.Add($r.lastPlayed.ToString('yyyy-MM-dd HH:mm'))
            [void]$it.SubItems.Add([string]$r.games)
            $blocked = @($state.bl.blocked | Where-Object { $_.code -eq $r.code }).Count -gt 0
            [void]$it.SubItems.Add($(if ($blocked) { 'blocked' } else { '' }))
            if ($blocked) { $it.ForeColor = [System.Drawing.Color]::Gray }
            [void]$lv.Items.Add($it)
        }
        $dlg.Controls.Add($lv)
        $blockBtn = New-Object System.Windows.Forms.Button
        $blockBtn.Text = 'Block selected'
        $blockBtn.Location = New-Object System.Drawing.Point(12, 324)
        $blockBtn.Size = New-Object System.Drawing.Size(150, 30)
        $blockBtn.Anchor = 'Bottom,Left'
        $dlg.Controls.Add($blockBtn)
        $closeBtn = New-Object System.Windows.Forms.Button
        $closeBtn.Text = 'Close'
        $closeBtn.Location = New-Object System.Drawing.Point(432, 324)
        $closeBtn.Size = New-Object System.Drawing.Size(100, 30)
        $closeBtn.Anchor = 'Bottom,Right'
        $closeBtn.Add_Click({ $dlg.Close() })
        $dlg.Controls.Add($closeBtn)
        $dlg.CancelButton = $closeBtn
        $blockBtn.Add_Click({
            $sel = @($lv.SelectedItems)
            if ($sel.Count -eq 0) { return }
            $added = @()
            foreach ($it in $sel) {
                $note = "played $($it.SubItems[2].Text)"
                if ($it.SubItems[1].Text) { $note = "$($it.SubItems[1].Text), $note" }
                if (Add-BlockedCode $state.bl $it.Text $note) { $added += $it.Text }
                $it.SubItems[4].Text = 'blocked'; $it.ForeColor = [System.Drawing.Color]::Gray
            }
            & $save $(if ($added.Count) { "Blocked $($added -join ', ')." } else { 'Already blocked.' })
        })
        [void]$dlg.ShowDialog($form)
    })

    & $refresh
    [void]$form.ShowDialog()
}

# --------------------------------------------------------------------------- #
# Entry point
# --------------------------------------------------------------------------- #
$userSlippiDir = Get-SlippiUserDir $LauncherDir
if (-not $BlocklistPath) { $BlocklistPath = Join-Path $userSlippiDir 'blocklist.json' }
if (-not $ReplayDir) { $ReplayDir = Get-ReplayDir $userSlippiDir }
$ownCode = Get-OwnConnectCode $userSlippiDir

$cli = $List -or $Add -or $Remove -or $Recent -or $Enable -or $Disable -or ($null -ne $Mode)
if (-not $cli) {
    Show-Gui $BlocklistPath $ReplayDir $ownCode
    exit 0
}

$bl = Read-Blocklist $BlocklistPath
$changed = $false

if ($Add) {
    $norm = ConvertTo-NormalizedCode $Add
    if ($ownCode -and $norm -eq $ownCode) { Write-Host "error: $norm is your own connect code."; exit 1 }
    try {
        if (Add-BlockedCode $bl $Add $Note) { Write-Host "Added $norm"; $changed = $true }
        else { Write-Host "$norm was already blocked"; if ($Note) { $changed = $true } }
    } catch {
        Write-Host "error: $($_.Exception.Message)"
        exit 1
    }
}
if ($Remove) {
    if (Remove-BlockedCode $bl $Remove) { Write-Host "Removed $(ConvertTo-NormalizedCode $Remove)"; $changed = $true }
    else { Write-Host "$(ConvertTo-NormalizedCode $Remove) was not on the list" }
}
if ($Enable) { $bl.enabled = $true; $changed = $true; Write-Host 'Blocklist enabled' }
if ($Disable) { $bl.enabled = $false; $changed = $true; Write-Host 'Blocklist disabled' }
if ($null -ne $Mode) {
    # accept both  -Mode unranked,teams  and  -Mode unranked -Mode teams  (and "unranked, teams")
    $wanted = @($Mode | ForEach-Object { "$_" -split ',' } | ForEach-Object { $_.ToLowerInvariant().Trim() } | Where-Object { $_ })
    $bad = @($wanted | Where-Object { $_ -notin $script:ValidModes })
    if ($bad.Count) { Write-Host "error: Unknown mode(s): $($bad -join ', '). Valid: $($script:ValidModes -join ', ')"; exit 1 }
    $bl.modes = @($wanted | Select-Object -Unique); $changed = $true
    Write-Host "Modes set to: $($bl.modes -join ', ')"
}
if ($changed) { Write-Blocklist $BlocklistPath $bl; Write-Host "Saved $BlocklistPath" }

if ($List -or $changed) {
    Write-Host ""
    Write-Host "Blocklist: $BlocklistPath"
    Write-Host "Enabled:   $($bl.enabled)    Modes: $($bl.modes -join ', ')    Requeue delay: $($bl.requeueDelayMs) ms"
    if (@($bl.blocked).Count -eq 0) { Write-Host "(no codes blocked)" }
    else { $bl.blocked | Format-Table -AutoSize code, note | Out-String | Write-Host }
}

if ($Recent) {
    Write-Host "Recent opponents from $ReplayDir (you: $(if ($ownCode) { $ownCode } else { 'unknown' }))"
    $r = @(Get-RecentOpponents $ReplayDir $Count $ownCode)
    if ($r.Count -eq 0) { Write-Host "(no online replays found)" }
    else { $r | Format-Table -AutoSize code, name, lastPlayed, games | Out-String | Write-Host }
}
exit 0
