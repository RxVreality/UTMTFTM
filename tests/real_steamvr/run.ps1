# Runs the real SteamVR with its null headset and a pretend controller, in its own settings and its own driver list, so nothing of
# J4's is read or written, and tries the add-on's button hook against it. Waits its turn for the PC first. No headset needed.
# Result: build\rt\result.txt, ending in "finished". after.ps1 closes everything, because the PC's guard takes this SteamVR for J4
# playing and cuts this script as soon as the check program ends. That's expected, and the other jobs wait about a minute for it.
# -Gate, or UTMTFTM_HEAVY_GATE, is an optional script that queues heavy jobs on a busy PC. Without one it just runs.
param([string]$Root = (Resolve-Path "$PSScriptRoot\..\..").Path, [string]$Gate = $env:UTMTFTM_HEAVY_GATE)
$ErrorActionPreference = 'Continue'
$useGate = $Gate -and (Test-Path $Gate)
$rt = "$Root\build\rt"
$out = "$rt\result.txt"
New-Item -ItemType Directory -Force "$rt\config", "$rt\logs", "$rt\localapp", "$rt\testctl\bin\win64", "$rt\testctl\resources\settings" | Out-Null
"waiting for its turn on the PC" | Set-Content $out

$owner = $PID
if ($useGate) { & powershell -NoProfile -ExecutionPolicy Bypass -File $Gate -session "UTMTFTM" -what "real SteamVR hook check" -owner $owner -ram_gb 4 }
if ($useGate -and $LASTEXITCODE -ne 0) { "gave up waiting for the PC`nfinished" | Set-Content $out; exit 4 }

$running = @(Get-Process vrserver, vrcompositor, vrmonitor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
  "SteamVR is already running here. Not touching it.`nfinished" | Set-Content $out
  if ($useGate) { & powershell -NoProfile -ExecutionPolicy Bypass -File $Gate -release -owner $owner }
  exit 3
}

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
$steamvr = 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR'
$paths = @{ config = @("$rt\config"); external_drivers = @("$Root\dist\driver", "$rt\testctl"); jsonid = 'vrpathreg'; log = @("$rt\logs"); runtime = @($steamvr); version = 1 } | ConvertTo-Json
$paths | Set-Content "$rt\openvrpaths.vrpath" -Encoding ascii

$env:VR_PATHREG_OVERRIDE = "$rt\openvrpaths.vrpath"
$env:VR_CONFIG_PATH = "$rt\config"
$env:VR_LOG_PATH = "$rt\logs"
$env:LOCALAPPDATA = "$rt\localapp"
Set-Location "$Root\dist\real_steamvr"
$started = (Get-Date).AddSeconds(-1).ToString('o')
"start $(Get-Date)" | Set-Content $out
$proc = Start-Process -FilePath "$Root\dist\real_steamvr\real_hook_check.exe" -RedirectStandardOutput "$rt\check_out.txt" -RedirectStandardError "$rt\check_err.txt" -PassThru -WindowStyle Hidden

# Started apart from this script, so it isn't cut along with it.
$cmd = "powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSScriptRoot\after.ps1`" -CheckPid $($proc.Id) -Owner $owner -Rt `"$rt`" -Gate `"$Gate`" -Started `"$started`""
Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd } | Out-Null

for ($i = 0; $i -lt 300; $i++) {
  if (Select-String -Path $out -Pattern '^finished$' -Quiet) { break }
  Start-Sleep -Seconds 1
}
