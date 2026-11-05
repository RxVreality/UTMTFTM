# Packs dist\ into what a release ships: an installer and a zip, each with a SHA-256 file beside it.
# Run build.cmd first. Needs NSIS. Usage: packaging\make-release.ps1 -Version 0.1.0
param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot
$dist = Join-Path $root 'dist'
$out = Join-Path $root 'build\release'
$stage = Join-Path $out 'UTMTFTM'

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'driver\bin\win64') | Out-Null

foreach ($f in 'utmtftm.exe', 'openvr_api.dll', 'utmtftm.vrmanifest', 'utmtftm.png', 'install.cmd', 'uninstall.cmd') { Copy-Item (Join-Path $dist $f) $stage }
Copy-Item (Join-Path $dist 'input') (Join-Path $stage 'input') -Recurse
Copy-Item (Join-Path $dist 'driver\driver.vrdrivermanifest') (Join-Path $stage 'driver')
Copy-Item (Join-Path $dist 'driver\resources') (Join-Path $stage 'driver\resources') -Recurse
Copy-Item (Join-Path $dist 'driver\bin\win64\driver_utmtftm.dll') (Join-Path $stage 'driver\bin\win64')
Copy-Item (Join-Path $root 'LICENSE'), (Join-Path $root 'THIRD_PARTY_NOTICES.md') $stage

# The installer's icon: an .ico file with the program's one PNG inside it.
$png = [IO.File]::ReadAllBytes((Join-Path $dist 'utmtftm.png'))
$ico = Join-Path $out 'utmtftm.ico'
$w = New-Object IO.BinaryWriter([IO.File]::Create($ico))
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]1)
$w.Write([byte]0); $w.Write([byte]0); $w.Write([byte]0); $w.Write([byte]0)
$w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$png.Length); $w.Write([uint32]22)
$w.Write($png)
$w.Close()

$zip = Join-Path $out "UTMTFTM-$Version.zip"
Compress-Archive -Path $stage -DestinationPath $zip

$setup = Join-Path $out "UTMTFTM-Setup-$Version.exe"
$nsis = Get-Command makensis -ErrorAction SilentlyContinue
$nsis = if ($nsis) { $nsis.Source } else { Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe' }
& $nsis /V2 "/DVERSION=$Version" "/DSTAGE=$stage" "/DOUTFILE=$setup" "/DICON=$ico" (Join-Path $PSScriptRoot 'installer.nsi')
if ($LASTEXITCODE -ne 0) { throw "makensis failed with code $LASTEXITCODE" }

foreach ($f in $zip, $setup) {
  $hash = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower()
  "$hash  $(Split-Path $f -Leaf)" | Set-Content "$f.sha256" -Encoding Ascii
}
Get-ChildItem $out -File | Where-Object { $_.Extension -ne '.ico' } | ForEach-Object { '{0,10}  {1}' -f $_.Length, $_.Name }
