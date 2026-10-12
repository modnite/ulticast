# Fetches the vendored dependencies into third_party/:
#   ffmpeg/          BtbN LGPL shared build (muxing + AAC + RTMP)
#   nv-codec-headers NVENC API headers (n13.1)
#   cef/             CEF 150 standard distribution (browser overlays)
#
#   powershell -ExecutionPolicy Bypass -File tools\fetch-deps.ps1
#
# Run once before configuring CMake on a fresh clone. CI runs this too.

param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$third = Join-Path $Root 'third_party'
New-Item -ItemType Directory -Force -Path $third | Out-Null
$tmp = Join-Path $env:TEMP 'ulticast-deps'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

function Expand-TarBz2([string]$archive, [string]$destination) {
    # Prefer 7-Zip (present on CI runners), then tar, then Python.
    $sevenZip = @(
        "$env:ProgramFiles\7-Zip\7z.exe",
        "${env:ProgramFiles(x86)}\7-Zip\7z.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($sevenZip) {
        & $sevenZip x -y "-o$tmp" $archive | Out-Null
        $tar = Get-ChildItem $tmp -Filter *.tar | Select-Object -First 1
        & $sevenZip x -y "-o$tmp\cef-extract" $tar.FullName | Out-Null
        return (Join-Path $tmp 'cef-extract')
    }
    try {
        & tar.exe -xjf $archive -C $destination 2>$null
        if ($LASTEXITCODE -eq 0) { return $destination }
    } catch { }
    python -c "import tarfile,sys; tarfile.open(sys.argv[1],'r:bz2').extractall(sys.argv[2], filter='fully_trusted')" $archive $destination
    return $destination
}

# ---------------------------------------------------------------- FFmpeg
if (-not (Test-Path (Join-Path $third 'ffmpeg\include'))) {
    Write-Host 'Fetching FFmpeg (BtbN LGPL shared)...'
    $zip = Join-Path $tmp 'ffmpeg-shared.zip'
    curl.exe -fL --retry 3 -o $zip 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip'
    $extract = Join-Path $tmp 'ff'
    if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
    Expand-Archive $zip -DestinationPath $extract -Force
    $src = (Get-ChildItem $extract -Directory | Select-Object -First 1).FullName
    Move-Item $src (Join-Path $third 'ffmpeg')
} else {
    Write-Host 'FFmpeg already present.'
}

# --------------------------------------------------------- nv-codec-headers
if (-not (Test-Path (Join-Path $third 'nv-codec-headers\include'))) {
    Write-Host 'Fetching nv-codec-headers (n13.1)...'
    git clone --depth 1 --branch n13.1.15.0 https://github.com/FFmpeg/nv-codec-headers.git (Join-Path $third 'nv-codec-headers')
} else {
    Write-Host 'nv-codec-headers already present.'
}

# -------------------------------------------------------------------- CEF
if (-not (Test-Path (Join-Path $third 'cef\include'))) {
    $cefName = 'cef_binary_150.0.22+gc1a097e+chromium-150.0.7871.256_windows64.tar.bz2'
    Write-Host "Fetching CEF ($cefName) -- about 330 MB..."
    $archive = Join-Path $tmp $cefName
    curl.exe -fL --retry 3 -o $archive "https://cef-builds.spotifycdn.com/$cefName"
    Write-Host 'Extracting CEF...'
    $extractRoot = Join-Path $tmp 'cef-extract'
    if (Test-Path $extractRoot) { Remove-Item -Recurse -Force $extractRoot }
    New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
    $out = Expand-TarBz2 $archive $extractRoot
    # Find the top-level cef_binary_* directory and move its contents into third_party/cef
    $top = Get-ChildItem $out -Directory | Select-Object -First 1
    $dest = Join-Path $third 'cef'
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    foreach ($item in Get-ChildItem $top.FullName) {
        Move-Item $item.FullName (Join-Path $dest $item.Name)
    }
} else {
    Write-Host 'CEF already present.'
}

Write-Host ''
Write-Host 'Dependencies ready:'
Get-ChildItem $third | Select-Object -ExpandProperty Name
