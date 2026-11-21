# Co-op test runner for one PC (run it on the host; the other PC just needs the game running and
# connected, see docs\TESTING.md). It starts the game, waits for the other player, plays one scenario
# through the mod's test commands and prints the log lines that matter.
#
#   tools\coop-test.ps1 -Scenario race                 # event 15.1.1, AI stopped so the host wins
#   tools\coop-test.ps1 -Scenario boss -Event 15.1.2   # rival race, rival drives
#   tools\coop-test.ps1 -Scenario list                 # show all scenarios
#
# Needs [Debug] TestKeys=1 in scripts\MWTogether.ini on the PC it runs on. -Solo skips waiting for the
# other player. -Keep leaves the game running at the end.
param(
    [string]$Scenario = 'list',
    [string]$Event = '',
    [string]$Game = (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent),
    [switch]$Solo,
    [switch]$Keep,
    [int]$PeerTimeoutSec = 420
)
$ErrorActionPreference = 'Stop'
$log = Join-Path $Game 'scripts\MWTogether.log'
$cmdFile = Join-Path $Game 'scripts\MWTogether.cmd'

$scenarios = [ordered]@{
    smoke    = 'Start, reach free roam, connect. Pass: "Ghost acquired", no CRASH.'
    race     = 'Start an event (default 15.1.1), stop the AI after 20 s so this PC wins. Pass: other PC logs "the other player won - counted as our win too" and its cash goes up.'
    boss     = 'Rival race (default 15.1.2), AI keeps driving. Pass: standings on each PC show that player + the rival; the partner''s car is visible next to the race.'
    restart  = 'Start an event, restart it after 40 s. Pass: other PC logs "peer restarted the race - restarting here too".'
    cutscene = 'Start an event and press skip in the intro on this PC only. Pass: intro plays in full (~7.7 s). Run "skipnis" on the other PC too to see it skip.'
    pursuit  = 'Start a pursuit on this PC in free roam. Pass: other PC logs "Cops: showing the other player''s police car".'
    ram      = 'Park 25 m from the other car, then hit it at 90 km/h. Pass: this PC logs "hit the other player, lost N km/h", the other "pushed by N km/h" and its car moves several metres.'
    together = 'Start an event; set [Debug] ReadyDelayMs=6000 on the OTHER PC first. Pass: this PC logs "on the grid first - waiting" then "go (both ready, waited 6.x s)".'
    results  = 'Race 15.1.1 (this PC wins), leave the results screen here, start 15.1.2 while the other PC still shows its results. Pass: other PC logs "invited while we''re on the results screen - leaving it to join" and joins 15.1.2.'
    kinds    = 'Smoke-run tollbooth 14.4.1, speedtrap 13.5.1, drag 12.7.3, knockout 15.3.1 (70 s each, game restarted in between; the other PC must restart and reconnect each time). Pass: no CRASH on either PC.'
    solo     = 'Race alone (use -Solo). Pass: the win pays out and nothing about co-op shows up.'
}
if ($Scenario -eq 'list' -or -not $scenarios.Contains($Scenario)) {
    'Scenarios:'; $scenarios.GetEnumerator() | ForEach-Object { '  {0,-9} {1}' -f $_.Key, $_.Value }; return
}

function Read-Log { if (-not (Test-Path $log)) { return '' }; $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); $sr = New-Object IO.StreamReader($fs); $t = $sr.ReadToEnd(); $sr.Close(); $t }
function Send-Cmd([string]$c) {   # waits until the mod took the previous command, so none is overwritten
    $end = (Get-Date).AddSeconds(10); while ((Test-Path $cmdFile) -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 200 }
    Add-Content $cmdFile $c; Start-Sleep -Milliseconds 900
}
function Wait-Log([string]$pattern, [int]$sec) {
    $end = (Get-Date).AddSeconds($sec)
    do { Start-Sleep 2; $t = Read-Log } until ($t -match $pattern -or (Get-Date) -gt $end)
    $t -match $pattern
}
function Start-Game {
    Stop-Process -Name speed -Force -ErrorAction SilentlyContinue; Start-Sleep 4
    Remove-Item $log -ErrorAction SilentlyContinue   # never match the previous run's lines
    Start-Process (Join-Path $Game 'speed.exe') -WorkingDirectory $Game -WindowStyle Minimized
    if (-not (Wait-Log 'flow state 5 -> 6' 120)) { throw 'The game did not reach free roam (check [Start] options and the log).' }
    if (-not $Solo) { if (-not (Wait-Log 'Ghost acquired' $PeerTimeoutSec)) { throw 'The other player did not show up.' } }
}
function Show-Log([string]$pattern, [int]$last = 14) {
    (Read-Log) -split "`n" | Where-Object { $_ -match $pattern -and $_ -notmatch 'Perf:|watchdog' } | Select-Object -Last $last
}
$always = 'CRASH|exception|Command'

try {
    switch ($Scenario) {
        'smoke'    { Start-Game; Start-Sleep 10; Show-Log "$always|Connected|Ghost acq|Notice|flow state 5" }
        'race'     { if (-not $Event) { $Event = '15.1.1' }; Start-Game; Start-Sleep 20; Send-Cmd 'cash'; Send-Cmd "event $Event"
                     Wait-Log 'peer racer info|HUD_SingleRace' 60 | Out-Null; Start-Sleep 20; Send-Cmd 'slowai'
                     Wait-Log 'Race: we finished|FE screens: \[(PostRace|InGamePhoto)' 420 | Out-Null; Start-Sleep 6; Send-Cmd 'cash'
                     Show-Log "$always|Event: s|Race: (the|AI racer|opponent)|Start: (on|go)|Notice|FE screens: \[Post" 18 }
        'boss'     { if (-not $Event) { $Event = '15.1.2' }; Start-Game; Start-Sleep 20; Send-Cmd "event $Event"
                     Wait-Log 'Race: we finished|FE screens: \[(PostRace|InGamePhoto)' 420 | Out-Null; Start-Sleep 6; Send-Cmd 'cash'
                     Show-Log "$always|Event: s|Race:|Ghost acq|Notice|FE screens: \[Post" 18 }
        'restart'  { if (-not $Event) { $Event = '15.1.1' }; Start-Game; Start-Sleep 20; Send-Cmd "event $Event"
                     Wait-Log 'peer racer info|HUD_SingleRace' 60 | Out-Null; Start-Sleep 40; Send-Cmd 'restart'; Start-Sleep 40
                     Show-Log "$always|Race: (restarted|peer restarted)" }
        'cutscene' { if (-not $Event) { $Event = '15.1.1' }; Start-Game; Start-Sleep 20; Send-Cmd "event $Event"
                     Wait-Log '\(cutscene\)' 20 | Out-Null; Start-Sleep -Milliseconds 800; Send-Cmd 'skipnis'; Start-Sleep 20
                     Show-Log "$always|Cutscene|Event: starting|FE screens: \[HUD" }
        'pursuit'  { Start-Game; Start-Sleep 40; Send-Cmd 'pursuit'; Start-Sleep 60; Send-Cmd 'uncop'; Start-Sleep 20
                     Show-Log "$always|Pursuit:|Cops: showing|UI: closing" 16 }
        'ram'      { Start-Game; Start-Sleep 45; Send-Cmd 'autopilot'; Send-Cmd 'ram'; Start-Sleep 5; Send-Cmd 'ram'; Start-Sleep 5; Send-Cmd 'ram'; Start-Sleep 3; Send-Cmd 'pos'; Start-Sleep 8
                     Show-Log "$always|Collision:|protection" }
        'together' { if (-not $Event) { $Event = '15.1.1' }; Start-Game; Start-Sleep 20; Send-Cmd "event $Event"; Start-Sleep 50
                     Show-Log "$always|Start: (on|go)|Event: starting|FE screens: \[HUD" }
        'results'  { Start-Game; Start-Sleep 20; Send-Cmd 'event 15.1.1'; Wait-Log 'peer racer info|HUD_SingleRace' 60 | Out-Null; Start-Sleep 20; Send-Cmd 'slowai'
                     Wait-Log 'Race: we finished|FE screens: \[(PostRace|InGamePhoto)' 420 | Out-Null; Start-Sleep 8; Send-Cmd 'accept InGamePhotoMaster.fng'; Send-Cmd 'accept PostRace_Results.fng'; Start-Sleep 25
                     Send-Cmd 'event 15.1.2'; Start-Sleep 60
                     Show-Log "$always|Event: |FE screens|Race: (we|the)" 16 }
        'kinds'    { foreach ($ev in '14.4.1', '13.5.1', '12.7.3', '15.3.1') {
                         Start-Game; Start-Sleep 20; Send-Cmd "event $ev"; Start-Sleep 40; Send-Cmd "shot $env:TEMP\mwt-kinds-$ev.bmp"; Start-Sleep 35
                         "== $ev (game still running: $([bool](Get-Process speed -ErrorAction SilentlyContinue)))"
                         Show-Log "$always|Event: (race|starting)|Race: (AI racer|peer racer)|Traffic: " 8 } }
        'solo'     { if (-not $Event) { $Event = '15.1.1' }; $Solo = $true; Start-Game; Start-Sleep 10; Send-Cmd 'cash'; Send-Cmd "event $Event"
                     Start-Sleep 35; Send-Cmd 'slowai'; Wait-Log 'Race: we finished|FE screens: \[(PostRace|InGamePhoto)' 420 | Out-Null; Start-Sleep 15; Send-Cmd 'cash'
                     Show-Log "$always|Start: (on|go)|Collision|Race: the|FE screens: \[Post" }
    }
}
finally {
    if (-not $Keep) { Stop-Process -Name speed -Force -ErrorAction SilentlyContinue }
}
