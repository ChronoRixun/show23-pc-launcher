<#
.SYNOPSIS
    Packages a built Release launcher as a portable zip: unzip anywhere, run show23-pc.exe.

.DESCRIPTION
    The launcher loads CEF and its UI from its data folder. In portable mode (the portable.marker file next to
    the exe's show23-pc folder) that folder lives beside the executable, so a zip in this layout is a
    complete, self-contained launcher:

        show23-pc.exe
        show23-pc\portable.marker
        show23-pc\data\cef\release\...       the CEF runtime the build produced
        show23-pc\data\launcher-ui\...        the UI (src\launcher-ui)

    Run this after a Release build to write a ZIP, SHA256SUMS.txt and launcher.json locally.

.EXAMPLE
    tools\package-portable.ps1 -Version 0.1.0          # -> dist\show23-pc-0.1.0-win64-portable.zip
#>
param(
    [Parameter(Mandatory)] [string] $Version,
    [string] $OutDir = '',
    [string] $MinLauncher = '0.1.0'
)

$ErrorActionPreference = 'Stop'
$versionPattern = '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[a-z0-9-]+(\.[a-z0-9-]+)*)?$'
foreach ($value in @($Version, $MinLauncher)) {
    if ($value.Length -gt 48 -or $value -cnotmatch $versionPattern -or $value -match '-0\d|\.0\d') {
        throw "Expected a version like 1.2.3 or 1.2.3-beta.1."
    }
}
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist' }

$exe = Join-Path $repo 'build\bin\x64\Release\show23-pc.exe'
$cef = Join-Path $repo 'build\runtime\x64\Release\cef'
$ui = Join-Path $repo 'src\launcher-ui'
foreach ($p in $exe, $cef, $ui) { if (-not (Test-Path $p)) { throw "missing $p - build the Release configuration first" } }

$name = "show23-pc-$Version-win64-portable"
$stage = Join-Path $OutDir "stage\$name"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$data = Join-Path $stage 'show23-pc\data'
New-Item -ItemType Directory -Force -Path (Join-Path $data 'cef\release'), (Join-Path $data 'launcher-ui') | Out-Null

Copy-Item $exe (Join-Path $stage 'show23-pc.exe')
Set-Content -Path (Join-Path $stage 'show23-pc\portable.marker') -Value '' -NoNewline
Copy-Item "$cef\*" (Join-Path $data 'cef\release') -Recurse -Force
Copy-Item "$ui\*" (Join-Path $data 'launcher-ui') -Recurse -Force

$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
# tar.exe (bsdtar, in every supported Windows) keeps the top-level folder and is faster than Compress-Archive.
Push-Location (Join-Path $OutDir 'stage')
try {
    & tar.exe -a -cf $zip $name
    if ($LASTEXITCODE -ne 0) { throw "Could not create the portable ZIP." }
} finally { Pop-Location }
Remove-Item (Join-Path $OutDir 'stage') -Recurse -Force

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
# LF, no BOM: `sha256sum -c` on Linux / Git Bash rejects a CRLF line.
[IO.File]::WriteAllText((Join-Path $OutDir 'SHA256SUMS.txt'), "$hash  $name.zip`n", [Text.Encoding]::ASCII)
$size = (Get-Item $zip).Length
$manifest = [ordered] @{
    schema = 1
    part = 'launcher'
    version = $Version
    channel = $(if ($Version.Contains('-')) { 'beta' } else { 'stable' })
    min_launcher = $MinLauncher
    files = @(@{ path = "$name.zip"; size = $size; sha256 = $hash })
    compatibility = [ordered] @{ emulator = '*'; server = '*'; profile = '*' }
}
$json = $manifest | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText((Join-Path $OutDir 'launcher.json'), $json + "\`n", [Text.UTF8Encoding]::new($false))
Write-Host ("{0}`n  {1:n0} bytes, sha256 {2}" -f $zip, $size, $hash)
