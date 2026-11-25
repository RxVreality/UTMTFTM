# Packs dist\ into what a release ships: an installer and a zip, each with a SHA-256 file beside it.
# Run build.cmd first. Needs NSIS 3.08 or newer. Usage: packaging\make-release.ps1 -Version 0.1.0
#
# On its own it does everything in one go, unsigned. GitHub's release build runs it a step at a time, so the files can go
# off to be code-signed in between (README, "Code signing policy"):
#   -Step prepare   stage the files, and put the ones we sign in build\release\tosign: utmtftm.exe, driver_utmtftm.dll and
#                   the uninstaller
#   -Step pack      put build\release\tosign's files (signed by now, or not) back in, and make the zip and the installer
#   -Step sums      the .sha256 files, once the installer is signed too
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [ValidateSet('all', 'prepare', 'pack', 'sums')][string]$Step = 'all'
)
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot
$dist = Join-Path $root 'dist'
$out = Join-Path $root 'build\release'
$stage = Join-Path $out 'UTMTFTM'
$tosign = Join-Path $out 'tosign'
$ico = Join-Path $out 'utmtftm.ico'
$zip = Join-Path $out "UTMTFTM-$Version.zip"
$setup = Join-Path $out "UTMTFTM-Setup-$Version.exe"
$driverDir = Join-Path $stage 'driver\bin\win64'

# The installer's Details tab wants four numbers: "0.6.4" is 0.6.4.0, and a build that isn't a release ("0.0.0-1a2b3c4")
# is 0.0.0.0.
if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)') { throw "Version '$Version' doesn't start with a version like 0.6.4" }
$viNumbers = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"

function Invoke-Nsis([string[]]$extra) {
  $nsis = Get-Command makensis -ErrorAction SilentlyContinue
  $nsis = if ($nsis) { $nsis.Source } else { Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe' }
  & $nsis /V2 "/DVERSION=$Version" "/DVINUMBERS=$viNumbers" "/DSTAGE=$stage" "/DOUTFILE=$setup" "/DICON=$ico" @extra (Join-Path $PSScriptRoot 'installer.nsi')
  if ($LASTEXITCODE -ne 0) { throw "makensis failed with code $LASTEXITCODE" }
}

if ($Step -in 'all', 'prepare') {
  if (Test-Path $out) { Remove-Item $out -Recurse -Force }
  New-Item -ItemType Directory -Force $driverDir, $tosign | Out-Null

  foreach ($f in 'utmtftm.exe', 'openvr_api.dll', 'utmtftm.vrmanifest', 'utmtftm.png', 'install.cmd', 'uninstall.cmd') { Copy-Item (Join-Path $dist $f) $stage }
  Copy-Item (Join-Path $dist 'input') (Join-Path $stage 'input') -Recurse
  Copy-Item (Join-Path $dist 'driver\driver.vrdrivermanifest') (Join-Path $stage 'driver')
  Copy-Item (Join-Path $dist 'driver\resources') (Join-Path $stage 'driver\resources') -Recurse
  Copy-Item (Join-Path $dist 'driver\bin\win64\driver_utmtftm.dll') $driverDir
  Copy-Item (Join-Path $root 'LICENSE'), (Join-Path $root 'THIRD_PARTY_NOTICES.md') $stage

  # The installer's icon: an .ico file with the program's one PNG inside it.
  $png = [IO.File]::ReadAllBytes((Join-Path $dist 'utmtftm.png'))
  $w = New-Object IO.BinaryWriter([IO.File]::Create($ico))
  $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]1)
  $w.Write([byte]0); $w.Write([byte]0); $w.Write([byte]0); $w.Write([byte]0)
  $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$png.Length); $w.Write([uint32]22)
  $w.Write($png)
  $w.Close()

  # Our own files go to be signed. openvr_api.dll is Valve's and ships as Valve publishes it.
  Copy-Item (Join-Path $stage 'utmtftm.exe'), (Join-Path $driverDir 'driver_utmtftm.dll') $tosign
  Invoke-Nsis @("/DEXPORT_UNINST=$(Join-Path $tosign 'uninstall.exe')")
  Remove-Item "$setup.export-only.exe" -ErrorAction SilentlyContinue
  if (-not (Test-Path (Join-Path $tosign 'uninstall.exe'))) { throw "makensis didn't write the uninstaller" }
}

if ($Step -in 'all', 'pack') {
  foreach ($f in 'utmtftm.exe', 'driver_utmtftm.dll', 'uninstall.exe') {
    if (-not (Test-Path (Join-Path $tosign $f))) { throw "build\release\tosign\$f is missing: run -Step prepare first" }
  }
  Copy-Item (Join-Path $tosign 'utmtftm.exe') $stage -Force
  Copy-Item (Join-Path $tosign 'driver_utmtftm.dll') $driverDir -Force
  Remove-Item $zip, $setup -ErrorAction SilentlyContinue
  Compress-Archive -Path $stage -DestinationPath $zip
  Invoke-Nsis @("/DIMPORT_UNINST=$(Join-Path $tosign 'uninstall.exe')")
}

if ($Step -in 'all', 'sums') {
  foreach ($f in $zip, $setup) {
    $hash = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower()
    "$hash  $(Split-Path $f -Leaf)" | Set-Content "$f.sha256" -Encoding Ascii
  }
  Get-ChildItem $out -File | Where-Object { $_.Extension -ne '.ico' } | ForEach-Object { '{0,10}  {1}' -f $_.Length, $_.Name }
}
