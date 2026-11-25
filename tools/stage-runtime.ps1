[CmdletBinding()]
param(
    [string] $Version,
    [Parameter(Mandatory)] [string] $EnginePackage,
    [Parameter(Mandatory)] [string] $ServerFolder,
    [Parameter(Mandatory)] [string] $OutDir,
    [string] $ManualMode,
    [string] $ServerLicense
)

$ErrorActionPreference = 'Stop'
if (!$Version) { $Version = (Get-Content -LiteralPath "$PSScriptRoot/release-version.txt" -Raw).Trim() }
$stageArgs = @('--stage-only', '--version', $Version, '--engine', $EnginePackage,
               '--server', $ServerFolder, '--out', $OutDir)
if ($ManualMode) { $stageArgs += @('--manual-mode', $ManualMode) }
if ($ServerLicense) { $stageArgs += @('--server-license', $ServerLicense) }
& python -B (Join-Path $PSScriptRoot 'package-versioned.py') @stageArgs
if ($LASTEXITCODE -ne 0) { throw 'Could not stage the bundled runtime.' }
