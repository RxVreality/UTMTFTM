# Runs the real SteamVR with its null headset and a pretend controller, in its own settings and its own driver list, so nothing of
# J4's is read or written, and tries the add-on's button hook against it. Waits its turn for the PC first. No headset needed.
# Result: build\rt\result.txt
param([string]$Root = (Resolve-Path "$PSScriptRoot\..\..").Path, [string]$Gate = 'C:\Users\jorda\Documents\tideborn\tools\heavy_gate.ps1')
$ErrorActionPreference = 'Continue'
$rt = "$Root\build\rt"
$steamvr = 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR'
foreach ($p in 'vrserver', 'vrcompositor', 'vrmonitor') { if (Get-Process $p -ErrorAction SilentlyContinue) { "SteamVR is already running here. Not touching it." | Set-Content "$rt\result.txt"; exit 3 } }
New-Item -ItemType Directory -Force "$rt\config", "$rt\logs", "$rt\localapp", "$rt\testctl\bin\win64", "$rt\testctl\resources\settings" | Out-Null
Remove-Item "$rt\localapp\UTMTFTM" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$rt\logs\*" -Force -ErrorAction SilentlyContinue

'{ "alwaysActivate": true, "name": "testctl", "directory": "", "resourceOnly": false, "hmd_presence": [] }' | Set-Content "$rt\testctl\driver.vrdrivermanifest" -Encoding ascii
'{ "driver_testctl": { "enable": true } }' | Set-Content "$rt\testctl\resources\settings\default.vrsettings" -Encoding ascii
Copy-Item "$Root\dist\real_steamvr\driver_testctl.dll" "$rt\testctl\bin\win64\" -Force
@'
{
  "steamvr": { "requireHmd": false, "forcedDriver": "null", "activateMultipleDrivers": true, "enableHomeApp": false, "showAdvancedSettings": true },
  "driver_null": { "enable": true }
}
'@ | Set-Content "$rt\config\steamvr.vrsettings" -Encoding ascii
$paths = @{ config = @("$rt\config"); external_drivers = @("$Root\dist\driver", "$rt\testctl"); jsonid = 'vrpathreg'; log = @("$rt\logs"); runtime = @($steamvr); version = 1 } | ConvertTo-Json
$paths | Set-Content "$rt\openvrpaths.vrpath" -Encoding ascii

$owner = $PID
& powershell -NoProfile -ExecutionPolicy Bypass -File $Gate -session "UTMTFTM" -what "real SteamVR hook check" -owner $owner -ram_gb 4 -maxwait_min 30
if ($LASTEXITCODE -ne 0) { "gave up waiting for the PC" | Set-Content "$rt\result.txt"; exit 4 }
try {
  $env:VR_PATHREG_OVERRIDE = "$rt\openvrpaths.vrpath"
  $env:VR_CONFIG_PATH = "$rt\config"
  $env:VR_LOG_PATH = "$rt\logs"
  $env:LOCALAPPDATA = "$rt\localapp"
  Set-Location "$Root\dist\real_steamvr"
  $out = "$rt\result.txt"
  "start $(Get-Date)" | Set-Content $out
  $proc = Start-Process -FilePath "$Root\dist\real_steamvr\real_hook_check.exe" -RedirectStandardOutput "$rt\check_out.txt" -RedirectStandardError "$rt\check_err.txt" -PassThru -WindowStyle Hidden
  if (-not $proc.WaitForExit(180000)) { "timed out after 3 minutes" | Add-Content $out; $proc | Stop-Process -Force }
  "exit $($proc.ExitCode)" | Add-Content $out
  Get-Content "$rt\check_out.txt" -ErrorAction SilentlyContinue | Add-Content $out
  "--- the add-on's log" | Add-Content $out
  Get-Content "$rt\localapp\UTMTFTM\driver.log" -ErrorAction SilentlyContinue | Add-Content $out
  "--- vrserver, about drivers" | Add-Content $out
  Get-Content "$rt\logs\vrserver.txt" -ErrorAction SilentlyContinue | Select-String -Pattern 'utmtftm|testctl|driver_null|Driver .* loaded|Unable to load|crash|exception' | Select-Object -First 40 | ForEach-Object { "$_" } | Add-Content $out
} finally {
  foreach ($p in 'real_hook_check', 'vrmonitor', 'vrcompositor', 'vrdashboard', 'vrwebhelper', 'vrserver', 'vrstartup') { Get-Process $p -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
  & powershell -NoProfile -ExecutionPolicy Bypass -File $Gate -release -owner $owner
  "finished" | Add-Content "$rt\result.txt"
}
