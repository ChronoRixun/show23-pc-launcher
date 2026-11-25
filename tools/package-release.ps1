param(
    [string] $Version,
    [string] $MinLauncher,
    [Parameter(Mandatory)] [string] $EnginePackage,
    [Parameter(Mandatory)] [string] $ServerFolder,
    [string] $ManualMode,
    [string] $ServerLicense,
    [string] $OutDir = ''
)

$ErrorActionPreference = 'Stop'
if (!$Version) { $Version = (Get-Content -LiteralPath "$PSScriptRoot/release-version.txt" -Raw).Trim() }
if (!$MinLauncher) { $MinLauncher = $Version }
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'build\bin\x64\Release'
if (!$OutDir) { $OutDir = Join-Path $repo 'dist' }
foreach ($name in 'show23-pc.exe', 'show23-pc-bootstrap.exe') {
    $file = Get-Item -LiteralPath (Join-Path $bin $name)
    if ($file.VersionInfo.ProductVersion -cne $Version) {
        throw "Rebuild $name with SHOW23_PACKAGE_VERSION=$Version before packaging."
    }
}
$packageArgs = @('--version', $Version, '--min-launcher', $MinLauncher, '--engine', $EnginePackage,
                 '--server', $ServerFolder, '--out', $OutDir)
if ($ManualMode) { $packageArgs += @('--manual-mode', $ManualMode) }
if ($ServerLicense) { $packageArgs += @('--server-license', $ServerLicense) }
& python -B (Join-Path $PSScriptRoot 'package-versioned.py') @packageArgs
if ($LASTEXITCODE -ne 0) { throw 'Could not create the versioned release ZIP.' }
