param(
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $Emulator,
    [Parameter(Mandatory)] [string] $Server,
    [Parameter(Mandatory)] [string] $PythonEmbedZip,
    [Parameter(Mandatory)] [string] $EmulatorLicense,
    [Parameter(Mandatory)] [string] $EmulatorCommit,
    [string] $ServerLicense
)

$ErrorActionPreference = 'Stop'
if ($Version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' -or $Version.Length -gt 48) {
    throw 'Expected a stable version such as 0.1.0.'
}
if ($EmulatorCommit -notmatch '\A[a-fA-F0-9]{40}\z') { throw 'Supply the full 40-digit emulator source commit.' }

$repo = Split-Path -Parent $PSScriptRoot
$launcher = Join-Path $repo 'build\bin\x64\Release\show23-pc.exe'
$browser = Join-Path $repo 'build\runtime\x64\Release\cef'
$ui = Join-Path $repo 'src\launcher-ui'
$oss = Join-Path $repo 'deps'
$utf8 = [Text.UTF8Encoding]::new($false)
function Get-Plain([string] $Path, [bool] $Directory) {
    $item = Get-Item -LiteralPath $Path -Force
    for ($parent = $item; $parent; $parent = $(if ($parent.PSIsContainer) { $parent.Parent } else { $parent.Directory })) {
        if ($parent.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked path: $($parent.FullName)" }
    }
    if ($item.PSIsContainer -ne $Directory) { throw "Unexpected input type: $Path" }
    return $item
}
function Get-TreeFiles([string] $Path) {
    foreach ($item in Get-ChildItem -LiteralPath $Path -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked entry: $($item.FullName)" }
        if ($item.PSIsContainer) { Get-TreeFiles $item.FullName }
        else { $item }
    }
}
function Copy-Tree([string] $From, [string] $To, [string] $Pattern = '*') {
    $base = (Get-Plain $From $true).FullName.TrimEnd('\')
    foreach ($file in Get-TreeFiles $base) {
        if ($file.Name -notlike $Pattern) { continue }
        $target = Join-Path $To $file.FullName.Substring($base.Length + 1)
        [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
}
function Save-Text([string] $Path, [string] $Text) { [IO.File]::WriteAllText($Path, $Text, $utf8) }
function Digest($File) { (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant() }

$Emulator = (Get-Plain $Emulator $false).FullName
if ([IO.Path]::GetFileName($Emulator) -ine 'shadPS4.exe') { throw 'Supply shadPS4.exe.' }
$EmulatorLicense = (Get-Plain $EmulatorLicense $false).FullName
if ((Get-Content -LiteralPath $EmulatorLicense -Raw) -notmatch 'Version 2, June 1991') {
    throw 'Supply the intact emulator GPL-2.0 licence.'
}
$PythonEmbedZip = (Get-Plain $PythonEmbedZip $false).FullName
$Server = (Get-Plain $Server $true).FullName
if (!$ServerLicense) { $ServerLicense = Join-Path (Split-Path -Parent $Server) 'LICENSE' }
$ServerLicense = (Get-Plain $ServerLicense $false).FullName
foreach ($required in @($launcher, (Join-Path $browser 'libcef.dll'), (Join-Path $ui 'main.html'),
    (Join-Path $Server 'olserver.py'), (Join-Path $Server 'roster_decode.py'), (Join-Path $Server 'card_import.py'), (Join-Path $Server 'tables\ol-messages.csv'))) {
    Get-Plain $required $false | Out-Null
}
Get-Plain (Join-Path $Server 'replies') $true | Out-Null

$notices = [ordered] @{
    'launcher-GPL-3.0.txt' = (Join-Path $repo 'LICENSE')
    'emulator\LICENSE.txt' = $EmulatorLicense
    'server.txt' = $ServerLicense
    'OFL.txt' = (Join-Path $ui 'assets\fonts\OFL.txt')
    'browser.txt' = (Join-Path $oss 'cef\LICENSE.txt')
    'curl.txt' = (Join-Path $oss 'curl\COPYING')
    'libtomcrypt.txt' = (Join-Path $oss 'libtomcrypt\LICENSE')
    'libtommath.txt' = (Join-Path $oss 'libtommath\LICENSE')
    'miniz.txt' = (Join-Path $oss 'miniz\LICENSE')
    'rapidjson.txt' = (Join-Path $oss 'rapidjson\license.txt')
}
foreach ($notice in $notices.Values) { Get-Plain $notice $false | Out-Null }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($PythonEmbedZip)
$dist = Join-Path $repo 'dist'
# One zip serves new players and the launcher's self-update, which looks for this exact name.
$name = "show23-pc-$Version-win64-portable"
$stage = Join-Path $dist ('.release-' + [Guid]::NewGuid().ToString('N'))
$tempZip = "$stage.zip"
$zip = Join-Path $dist "$name.zip"
try {
    $archiveNames = @{}
    foreach ($entry in $archive.Entries) {
        if ($entry.FullName -cnotmatch '^[A-Za-z0-9_][A-Za-z0-9_.-]*[A-Za-z0-9_]$' -or
            $entry.FullName -match '^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\.|$)' -or $archiveNames.ContainsKey($entry.FullName)) {
            throw 'Expected a flat embeddable runtime ZIP with unique, ordinary file names.'
        }
        $archiveNames[$entry.FullName] = $true
    }
    foreach ($required in 'python.exe', 'LICENSE.txt') {
        if (!$archiveNames.ContainsKey($required)) { throw "Missing $required in the embeddable ZIP." }
    }
    if (!($archiveNames.Keys -like 'python3*._pth') -or !($archiveNames.Keys -like 'python3*.zip') -or
        !($archiveNames.Keys -like 'python3*.dll')) { throw 'The embeddable ZIP is missing its interpreter support files.' }
    if (Test-Path -LiteralPath $dist) { Get-Plain $dist $true | Out-Null }
    $payload = Join-Path $stage $name
    $app = Join-Path $payload 'show23-pc'
    $runtime = Join-Path $app "runtime\$Version"
    $manifests = Join-Path $app "manifests\$Version"
    $python = Join-Path $runtime 'server\python'
    $licenses = Join-Path $payload 'LICENSES'
    foreach ($dir in $python, (Join-Path $runtime 'emulator'), (Join-Path $runtime 'tools\profile'), $manifests,
        (Join-Path $runtime 'server\tables'), (Join-Path $licenses 'emulator')) {
        [IO.Directory]::CreateDirectory($dir) | Out-Null
    }

    Copy-Item -LiteralPath $launcher -Destination $payload
    Save-Text (Join-Path $app 'portable.marker') ''
    Copy-Tree $browser (Join-Path $app 'data\cef\release')
    Copy-Tree $ui (Join-Path $app 'data\launcher-ui')
    Copy-Item -LiteralPath $Emulator -Destination (Join-Path $runtime 'emulator\shadPS4.exe')
    foreach ($relative in 'olserver.py', 'roster_decode.py', 'card_import.py', 'tables\ol-messages.csv') {
        Copy-Item -LiteralPath (Join-Path $Server $relative) -Destination (Join-Path $runtime "server\$relative")
    }
    Copy-Tree (Join-Path $Server 'replies') (Join-Path $runtime 'server\replies') '*.xml'
    if (!(Test-Path -LiteralPath (Join-Path $runtime 'server\replies'))) { throw 'No reply XML files found.' }
    foreach ($entry in $archive.Entries) {
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $python $entry.FullName), $false)
    }
    $image = [IO.File]::ReadAllBytes((Join-Path $python 'python.exe'))
    if ($image.Length -lt 64 -or [BitConverter]::ToUInt16($image, 0) -ne 0x5a4d) { throw 'Invalid embedded runtime executable.' }
    $header = [BitConverter]::ToInt32($image, 60)
    if ($header -lt 64 -or $header -gt $image.Length - 6 -or [BitConverter]::ToInt32($image, $header) -ne 0x4550 -or
        [BitConverter]::ToUInt16($image, $header + 4) -ne 0x8664) { throw 'Expected an x64 embedded runtime.' }
    Save-Text (Join-Path $runtime 'tools\profile\defaults.txt') "The emulator supplies the first-run defaults.$([char]10)"

    foreach ($part in 'emulator', 'server', 'profile') {
        $dir = Join-Path $runtime $(if ($part -eq 'profile') { 'tools\profile' } else { $part })
        $files = @(Get-TreeFiles $dir | Sort-Object FullName | ForEach-Object {
            $relative = $_.FullName.Substring($dir.Length + 1).Replace('\', '/')
            if ($relative.Length -gt 200 -or $relative -cnotmatch '^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$' -or
                $relative -match '(^|/)\.|\.(/|$)|(^|/)(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\.|/|$)') { throw "Unsupported part path: $relative" }
            [ordered] @{ path = $relative; size = $_.Length; sha256 = (Digest $_.FullName) }
        })
        if (!$files.Count -or $files.Count -gt 2048) { throw "Invalid manifest file count for $part." }
        $compatibility = [ordered] @{}
        foreach ($other in 'launcher', 'emulator', 'server', 'profile') {
            if ($other -ne $part) { $compatibility[$other] = $(if ($other -eq 'launcher') { '>=0.1.0' } else { "=$Version" }) }
        }
        $manifest = [ordered] @{
            schema = 1; part = $part; version = $Version; channel = 'stable'; min_launcher = '0.1.0'
            files = $files; compatibility = $compatibility
        }
        $json = ($manifest | ConvertTo-Json -Depth 6 -Compress) + "`n"
        if ($utf8.GetByteCount($json) -gt 256KB) { throw "Manifest too large for $part." }
        Save-Text (Join-Path $manifests "$part.json") $json
    }
    Save-Text (Join-Path $app 'manifests\runtime.txt') "$Version`n"
    foreach ($entry in $notices.GetEnumerator()) {
        Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $licenses $entry.Key)
    }
    Copy-Item -LiteralPath (Join-Path $python 'LICENSE.txt') -Destination (Join-Path $licenses 'python.txt')

    $source = "https://github.com/ChronoRixun/show23-pc-emulator (branch release, commit $EmulatorCommit)"
    Save-Text (Join-Path $licenses 'emulator\SOURCE.txt') "$source`n"
    $readme = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'player-readme.txt') -Raw -Encoding UTF8
    Save-Text (Join-Path $payload 'README.txt') ($readme.Replace('@EMULATOR_SOURCE@', $source))
    $sums = foreach ($file in Get-TreeFiles $payload | Sort-Object FullName) {
        "$(Digest $file.FullName)  $($file.FullName.Substring($payload.Length + 1).Replace('\', '/'))"
    }
    Save-Text (Join-Path $payload 'SHA256SUMS.txt') (($sums -join "`n") + "`n")
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $tempZip, [IO.Compression.CompressionLevel]::Optimal, $false)
    $hash = Digest $tempZip
    $size = (Get-Item -LiteralPath $tempZip).Length
    foreach ($output in $zip, (Join-Path $dist 'SHA256SUMS.txt')) {
        if (Test-Path -LiteralPath $output) { Get-Plain $output $false | Out-Null }
    }
    if ([IO.File]::Exists($zip)) { [IO.File]::Replace($tempZip, $zip, [NullString]::Value) }
    else { [IO.File]::Move($tempZip, $zip) }
    Save-Text (Join-Path $dist 'SHA256SUMS.txt') "$hash  $name.zip`n"
    # The release manifest the launcher's updater reads (same shape as tools\package-portable.ps1 writes).
    $manifest = [ordered] @{
        schema = 1
        part = 'launcher'
        version = $Version
        channel = $(if ($Version.Contains('-')) { 'beta' } else { 'stable' })
        min_launcher = '0.1.0'
        files = @(@{ path = "$name.zip"; size = $size; sha256 = $hash })
        compatibility = [ordered] @{ emulator = '*'; server = '*'; profile = '*' }
    }
    Save-Text (Join-Path $dist 'launcher.json') (($manifest | ConvertTo-Json -Depth 4) + "`n")
    Write-Host "$zip ($size bytes, SHA-256 $hash)"
    Write-Host "Emulator source: $source"
} finally {
    $archive.Dispose()
    if ([IO.File]::Exists($tempZip)) { [IO.File]::Delete($tempZip) }
    if ([IO.Directory]::Exists($stage)) { [IO.Directory]::Delete($stage, $true) }
}
