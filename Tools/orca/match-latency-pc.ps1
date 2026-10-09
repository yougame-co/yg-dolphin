# AGENT TEST - match timing (match-latency.mjs) - not a player's app.
# match-latency-pc.ps1: the PC side of a Mac<->PC match timing run (match-latency-xplat.sh starts it
# over SSH; ORCA.md, "Match start timing"). One headless, muted Orca, SDL HIDAPI off, a test mark with
# no added delay (ORCA_TEST_NET_DELAY_MS=0), holding the PC's Orca run lock. It never starts while a
# player's own Orca (the YouGame app's) is open, and never touches one. Expects match-latency.mjs and
# a session folder (sess\<name>.cookie, ids.json) beside it, and the release tools' common.ps1
# (Initialize-ReleaseEnv for $Node and $Disc, Wait-OrcaLock, Exit-OrcaLock, Write-Log).
# Output: <this folder>\runs\<Out>\ (raw timeline, Orca log, meta.json) and runs\<Out>.log.
param([Parameter(Mandatory)][ValidateSet('casual', 'ranked', 'friends')][string]$Mode,
  [Parameter(Mandatory)][ValidateSet('brawl', 'pplus')][string]$Game,
  [Parameter(Mandatory)][string]$Out, [int]$Matches = 5, [long]$SearchAt = 0, [string]$Side = 'b',
  [string]$Name = 'di', [string]$FriendRoom = '', [Parameter(Mandatory)][string]$Exe,
  [Parameter(Mandatory)][string]$Build, [string]$Common = 'F:\src\orca-release\common.ps1',
  [string]$PPlus = 'C:\orca-ab\pplus\Project+ Netplay Launcher.dol', [int]$LockMinutes = 30,
  [int]$Hold = 2500, [int]$Gap = 0)
$ErrorActionPreference = 'Stop'
. $Common
Initialize-ReleaseEnv
$env:SDL_JOYSTICK_HIDAPI = '0'; $env:SDL_JOYSTICK_HIDAPI_GAMECUBE = '0'; $env:SDL_HIDAPI_LIBUSB = '0'
$x = $PSScriptRoot; New-Item -ItemType Directory -Force "$x\runs" | Out-Null
$log = "$x\runs\$Out.log"; Remove-Item $log -ErrorAction SilentlyContinue
$player = @(Get-Process Orca -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*\yougame-desktop\*' -or $_.Path -like '*\resources\runtimes\*' })
if ($player.Count) { "A PLAYER'S ORCA IS OPEN (pid $($player[0].Id)): not running" | Write-Log $log; exit 3 }
$slug = if ($Game -eq 'pplus') { 'project-rollback' } else { 'open-brawl' }
Wait-OrcaLock "AGENT TEST match timing $Mode $Game $Out (headless, $Build)" $LockMinutes | Write-Log $log
try {
  $env:ORCA_TEST_NET_DELAY_MS = '0'
  "start $(Get-Date -Format s) exe $Exe ($((Get-Item $Exe).VersionInfo.FileVersion)) mode $Mode game $Game side $Side" | Write-Log $log
  $ErrorActionPreference = 'Continue'
  $a = @("$x\match-latency.mjs", '--side', $Side, '--mode', $Mode, '--bin', $Exe, '--disc', $Disc, '--slug', $slug, '--sess', "$x\sess",
    "--$Side", $Name, '--orca-build', $Build, '--matches', $Matches, '--out', "$x\runs\$Out", '--hold', $Hold, '--video', 'Null')
  if ($SearchAt) { $a += @('--search-at', $SearchAt) }
  if ($Gap) { $a += @('--gap', $Gap) }
  if ($FriendRoom) { $a += @('--friend-room', $FriendRoom) }
  if ($Game -eq 'pplus') { $a += @('--pplus', $PPlus) }
  & $Node @a 2>&1 | ForEach-Object { "$_" } | Write-Log $log | Out-Null
  $code = $LASTEXITCODE
} finally {
  Remove-Item Env:ORCA_TEST_NET_DELAY_MS -ErrorAction SilentlyContinue
  Exit-OrcaLock
  $left = @(Get-Process Orca -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe })
  foreach ($p in $left) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; "killed leftover test Orca $($p.Id)" | Write-Log $log | Out-Null }
}
"match-latency-pc exit $code $(Get-Date -Format s)" | Write-Log $log | Out-Null
(Select-String -Path $log -Pattern '^MLAT ' | Select-Object -Last 1).Line
"match-latency-pc exit $code (log $log)"
exit $code
