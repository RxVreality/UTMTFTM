# Checks what make-release.ps1 made, before anything goes in a release:
#  - every Windows file of ours says ProductName UTMTFTM and the release's version in its Details tab (code signing's rule)
#  - the zip and the installer hold exactly the files in build\release\tosign, byte for byte, so whatever got signed is
#    what people download, the uninstaller inside the installer included
#  - with -Signed, all of them carry a signature from -Signer, and with -Trusted Windows also trusts it
# Needs 7-Zip to look inside the installer. Usage: packaging\check-release.ps1 -Version 0.6.4 [-Signed [-Trusted] [-Signer name]]
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [switch]$Signed,
  [switch]$Trusted,
  [string]$Signer = 'SignPath Foundation'
)
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot
$out = Join-Path $root 'build\release'
$tosign = Join-Path $out 'tosign'
$zip = Join-Path $out "UTMTFTM-$Version.zip"
$setup = Join-Path $out "UTMTFTM-Setup-$Version.exe"
$look = Join-Path $out 'check'
$bad = 0
function say([string]$m) { Write-Output $m }
function fail([string]$m) { Write-Output "FAIL $m"; $script:bad++ }

$seven = Get-Command 7z -ErrorAction SilentlyContinue
$seven = if ($seven) { $seven.Source } else { Join-Path $env:ProgramFiles '7-Zip\7z.exe' }
if (-not (Test-Path $seven)) { throw "7-Zip is needed to look inside the installer" }

if (Test-Path $look) { Remove-Item $look -Recurse -Force }
Expand-Archive $zip (Join-Path $look 'zip')
& $seven x -y "-o$(Join-Path $look 'setup')" $setup | Out-Null
if ($LASTEXITCODE -ne 0) { throw "7-Zip couldn't open $setup" }

function inside([string]$where, [string]$name) {
  $f = @(Get-ChildItem (Join-Path $look $where) -Recurse -File -Filter $name)
  if ($f.Count -ne 1) { fail "$where has $($f.Count) copies of $name, not 1"; return $null }
  $f[0].FullName
}

# What each signed file is, and where its copies are.
$files = @(
  @{ name = 'utmtftm.exe'; copies = @('zip', 'setup') },
  @{ name = 'driver_utmtftm.dll'; copies = @('zip', 'setup') },
  @{ name = 'uninstall.exe'; copies = @('setup') }
)
$all = @()
foreach ($f in $files) {
  $mine = Join-Path $tosign $f.name
  if (-not (Test-Path $mine)) { fail "build\release\tosign\$($f.name) is missing"; continue }
  $all += $mine
  $hash = (Get-FileHash $mine).Hash
  foreach ($c in $f.copies) {
    $p = inside $c $f.name
    if ($p) {
      if ((Get-FileHash $p).Hash -eq $hash) { say "ok   $($f.name) in the $c is the one in tosign" }
      else { fail "$($f.name) in the $c differs from build\release\tosign\$($f.name)" }
    }
  }
}
$all += $setup

foreach ($p in $all) {
  $n = Split-Path $p -Leaf
  $v = (Get-Item $p).VersionInfo
  if ($v.ProductName -eq 'UTMTFTM') { say "ok   $n ProductName UTMTFTM" } else { fail "$n ProductName is '$($v.ProductName)'" }
  if ($v.ProductVersion -eq $Version) { say "ok   $n ProductVersion $Version" } else { fail "$n ProductVersion is '$($v.ProductVersion)', not $Version" }
  if ($Signed) {
    $s = Get-AuthenticodeSignature $p
    if (-not $s.SignerCertificate) { fail "$n isn't signed"; continue }
    if ($s.SignerCertificate.Subject -match [regex]::Escape($Signer)) { say "ok   $n signed by $($s.SignerCertificate.Subject)" }
    else { fail "$n is signed by '$($s.SignerCertificate.Subject)', not $Signer" }
    if ($Trusted) {
      if ($s.Status -eq 'Valid') { say "ok   $n signature trusted by Windows" } else { fail "$n signature status $($s.Status): $($s.StatusMessage)" }
    }
  }
}

Remove-Item $look -Recurse -Force
if ($bad) { throw "$bad check(s) failed" }
say "all release checks passed"
