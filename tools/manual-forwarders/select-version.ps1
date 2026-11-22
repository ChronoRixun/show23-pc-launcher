$ErrorActionPreference = 'Stop'
$top = Split-Path -Parent $PSScriptRoot
$versions = Join-Path $top 'show23-pc\versions'
$info = New-Object Diagnostics.ProcessStartInfo
$info.FileName = Join-Path $top 'show23-pc.exe'
$info.Arguments = '--select-manual'
$info.WorkingDirectory = $top
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$folder = ''
try {
    $process = [Diagnostics.Process]::Start($info)
    try {
        $selection = $process.StandardOutput.ReadToEnd().Trim()
        $process.WaitForExit()
        $pattern = '\A(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})(-[a-z0-9-]+(\.[a-z0-9-]+)*)?(~([2-9]|[1-9][0-9]+))?\z'
        if ($process.ExitCode -eq 0 -and $selection.Length -le 59 -and $selection -cmatch $pattern -and
            (Test-Path -LiteralPath (Join-Path $versions "$selection\manual") -PathType Container)) {
            $folder = $selection
        }
    } finally { $process.Dispose() }
} catch {
    # Missing bootstrap or a failed verification uses the ordinary no-version path.
}
if (!$folder) {
    [Console]::Error.WriteLine('No verified launcher version is ready. Start the launcher or re-extract the complete ZIP.')
    exit 1
}
$folder
