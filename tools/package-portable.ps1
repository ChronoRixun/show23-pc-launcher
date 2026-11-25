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
# The portable ZIP and the in-app asset are the same release.
& (Join-Path $PSScriptRoot 'package-release.ps1') @PSBoundParameters
