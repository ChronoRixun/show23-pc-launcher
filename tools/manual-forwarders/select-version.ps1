$ErrorActionPreference = 'Stop'
$top = Split-Path -Parent $PSScriptRoot
$data = Join-Path $top 'show23-pc'
$versions = Join-Path $data 'versions'
$versionPattern = '\A(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})(-[a-z0-9-]+(\.[a-z0-9-]+)*)?(~([2-9]|[1-9][0-9]+))?\z'

function Test-Folder([string] $Name) {
    if (!$Name -or $Name.Length -gt 59 -or $Name -cnotmatch $versionPattern -or $Name -match '[-.]0[0-9]+([.~]|$)') { return $false }
    $slot = Join-Path $versions $Name
    foreach ($path in $data, $versions, $slot, (Join-Path $slot manual)) {
        $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        if (!$item -or !$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { return $false }
    }
    return $true
}

function Get-RecordedVersion {
    $path = Join-Path $versions 'pointer.txt'
    $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    if (!$item -or $item.PSIsContainer -or $item.Length -gt 8192 -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { return }
    try {
        $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            ([IO.FileShare]::Read -bor [IO.FileShare]::Delete))
        try {
            if ($stream.Length -gt 8192) { return }
            $bytes = New-Object byte[] ([int] $stream.Length)
            if ($stream.Read($bytes, 0, $bytes.Length) -ne $bytes.Length) { return }
            $text = [Text.Encoding]::ASCII.GetString($bytes)
        } finally { $stream.Dispose() }
    } catch [IO.IOException] { return }
    if ($text -cnotmatch '\A(?<body>show23-pc pointer 1\r?\ncurrent (?<current>[^ \r\n]+)\r?\nprevious [^ \r\n]+\r?\ntrial (?<trial>[^ \r\n]+)\r?\nstarts [0-9]+\r?\nlast-request [^ \r\n]+\r?\n(failed [^\r\n]+\r?\n){0,8})end (?<hash>[a-f0-9]{64})\r?\n?\z') { return }
    $current = $Matches.current
    $trial = $Matches.trial
    $expected = $Matches.hash
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $actual = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($Matches.body))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    if ($actual -cne $expected) { return }
    if (Test-Folder $current) { return $current }
    if ($current -ceq '-' -and (Test-Folder $trial)) { return $trial }
}

$folder = Get-RecordedVersion
if (!$folder) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = Join-Path $top 'show23-pc.exe'
    $info.Arguments = '--prepare'
    $info.WorkingDirectory = $top
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    try {
        $process = [Diagnostics.Process]::Start($info)
        try {
            $selection = $process.StandardOutput.ReadToEnd().Trim()
            $process.WaitForExit()
            if ($process.ExitCode -eq 0 -and (Test-Folder $selection)) { $folder = $selection }
        } finally { $process.Dispose() }
    } catch {
        # Missing bootstrap or failed process startup takes the ordinary no-version path.
    }
}
if (!$folder) {
    [Console]::Error.WriteLine('No launcher version is ready. Close the launcher if it is open, then start show23-pc.exe once or re-extract the ZIP.')
    exit 1
}
$folder
