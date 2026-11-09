# Closes what run.ps1 opened, whatever became of run.ps1. The PC's guard takes any SteamVR for J4 playing, keeps whatever started
# it, and cuts the gate's holder the moment the check program ends. So run.ps1 can't be relied on to reach its own clean-up, and a
# SteamVR left running holds up every other job on the PC. This runs apart from it, so the guard leaves it alone.
param([int]$CheckPid, [int]$Owner, [string]$Rt, [string]$Gate, [string]$Started)
$ErrorActionPreference = 'Continue'
$out = "$Rt\result.txt"
$steamvr = 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR'
$since = [datetime]::Parse($Started)

$check = Get-Process -Id $CheckPid -ErrorAction SilentlyContinue
if ($check) {
  if (-not $check.WaitForExit(240000)) {
    "timed out after 4 minutes" | Add-Content $out
    Stop-Process -Id $CheckPid -Force -ErrorAction SilentlyContinue
  }
}

# Only SteamVR's own programs, and only ones started since this run began.
function Ours {
  Get-CimInstance Win32_Process | Where-Object {
    $_.ExecutablePath -and $_.ExecutablePath.StartsWith($steamvr, [StringComparison]::OrdinalIgnoreCase) -and $_.CreationDate -ge $since
  }
}
for ($i = 0; $i -lt 10; $i++) {
  $left = @(Ours)
  if ($left.Count -eq 0) { break }
  foreach ($p in $left) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Seconds 1
}
"steamvr programs left open: $(@(Ours).Count)" | Add-Content $out

Get-Content "$Rt\check_out.txt" -ErrorAction SilentlyContinue | Add-Content $out
if (-not (Select-String -Path "$Rt\check_out.txt" -Pattern '^(PASSED|FAILED):' -Quiet)) { "NO RESULT: the check stopped before it finished, so the lines above aren't the whole story" | Add-Content $out }
"--- the add-on's log" | Add-Content $out
Get-Content "$Rt\localapp\UTMTFTM\driver.log" -ErrorAction SilentlyContinue | Add-Content $out
"--- vrserver, about drivers" | Add-Content $out
Get-Content "$Rt\logs\vrserver.txt" -ErrorAction SilentlyContinue | Select-String -Pattern 'driver utmtftm|driver testctl|Loaded server driver|Unable to load|crash|exception|Sending Quit|Kill process|OnMsg_LaunchApplication' | Select-Object -First 30 | ForEach-Object { $s = "$_"; $s.Substring(0, [Math]::Min(240, $s.Length)) } | Add-Content $out

if ($Gate -and (Test-Path $Gate)) { & powershell -NoProfile -ExecutionPolicy Bypass -File $Gate -release -owner $Owner }
"finished" | Add-Content $out
