$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
# Source snapshots used when the launcher is built from a plain source directory.
$pins = @(
    @{ Folder = 'curl';        Repo = 'curl/curl';           Ref = '9d3f878e59d155ee2b916550bdca531424643710'; Header = 'include/curl/curl.h' },
    @{ Folder = 'rapidjson';   Repo = 'Tencent/rapidjson';   Ref = '24b5e7a8b27f42fa16b96fc70aade9106cf7102f'; Header = 'include/rapidjson/document.h' },
    @{ Folder = 'libtommath';  Repo = 'libtom/libtommath';    Ref = '839ae9ea66718705fba2b5773d1bdfb2b457cea4'; Header = 'tommath.h' },
    @{ Folder = 'libtomcrypt'; Repo = 'libtom/libtomcrypt';   Ref = 'b1fa61d594f4a92ebb5e09e665464481a23bfe37'; Header = 'src/headers/tomcrypt.h' },
    @{ Folder = 'miniz';       Repo = 'richgel999/miniz';     Ref = '77d0dce8627735138c51770d1799a1ef48f2117d'; Header = 'miniz.h' }
)
if (Test-Path -LiteralPath (Join-Path $root '.git')) {
    & git -C $root submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize dependency submodules.' }
}

$cache = Join-Path $root 'build\deps-cache'
$null = New-Item -ItemType Directory -Force -Path $cache
foreach ($pin in $pins) {
    $target = Join-Path $root "deps\$($pin.Folder)"
    if (Test-Path -LiteralPath (Join-Path $target $pin.Header)) { continue }
    if (Test-Path -LiteralPath $target) { throw "An incomplete dependency already exists at $target." }
    $archive = Join-Path $cache "$($pin.Folder)-$($pin.Ref).zip"
    if (-not (Test-Path -LiteralPath $archive)) {
        $url = "https://codeload.github.com/$($pin.Repo)/zip/$($pin.Ref)"
        Write-Host "Downloading $($pin.Folder)"
        $partial = "$archive.partial"
        Invoke-WebRequest -UseBasicParsing -TimeoutSec 60 -Uri $url -OutFile $partial
        Move-Item -LiteralPath $partial -Destination $archive
    }
    $unpacked = Join-Path $cache "$($pin.Folder)-extracted"
    if (Test-Path -LiteralPath $unpacked) { Remove-Item -LiteralPath $unpacked -Recurse -Force }
    # The .NET call works the same in Windows PowerShell and PowerShell 7, whichever started this script.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($archive, $unpacked)
    $children = @(Get-ChildItem -LiteralPath $unpacked -Directory)
    if ($children.Count -ne 1 -or -not (Test-Path -LiteralPath (Join-Path $children[0].FullName $pin.Header))) {
        throw "Unexpected source archive layout for $($pin.Folder)."
    }
    Move-Item -LiteralPath $children[0].FullName -Destination $target
}
