param(
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $MinLauncher,
    [Parameter(Mandatory)] [string] $FrozenRuntimeZip,
    [Parameter(Mandatory)] [string] $FrozenRuntimeManifest,
    [string] $OutDir = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'build\bin\x64\Release'
if (!$OutDir) { $OutDir = Join-Path $repo 'dist' }
foreach ($name in 'show23-pc.exe', 'show23-pc-bootstrap.exe') {
    $file = Get-Item -LiteralPath (Join-Path $bin $name)
    if ($file.VersionInfo.ProductVersion -cne $Version) {
        throw "Rebuild $name with SHOW23_PACKAGE_VERSION=$Version before packaging."
    }
}
& python (Join-Path $PSScriptRoot 'package-versioned.py') $Version $MinLauncher $FrozenRuntimeZip $FrozenRuntimeManifest $OutDir
if ($LASTEXITCODE -ne 0) { throw 'Could not create the versioned release ZIP.' }
