param(
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $MinLauncher,
    [Parameter(Mandatory)] [string] $FrozenRuntimeZip,
    [Parameter(Mandatory)] [string] $FrozenRuntimeManifest,
    [string] $OutDir = ''
)

$ErrorActionPreference = 'Stop'
# The portable ZIP and the in-app asset are the same release.
& (Join-Path $PSScriptRoot 'package-release.ps1') @PSBoundParameters
