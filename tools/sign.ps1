# Signs the Ulticast binaries with an Authenticode certificate.
#
#   $env:ULTI_SIGN_PFX = "C:\certs\ulticast.pfx"
#   $env:ULTI_SIGN_PFX_PASSWORD = "hunter2"
#   powershell -ExecutionPolicy Bypass -File tools\sign.ps1
#
# or pass -PfxPath/-PfxPassword explicitly. Requires signtool.exe (Windows SDK),
# which the build machines already have. CI does the same thing automatically
# when the SIGNING_PFX_BASE64 / SIGNING_PFX_PASSWORD repository secrets are set.
param(
    [string]$Path = "build\Release",
    [string]$PfxPath = $env:ULTI_SIGN_PFX,
    [string]$PfxPassword = $env:ULTI_SIGN_PFX_PASSWORD,
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [string[]]$Files = @("ulticast.exe", "ulticast-ui.exe")
)

$ErrorActionPreference = 'Stop'

if (-not $PfxPath -or -not (Test-Path $PfxPath)) {
    throw "PFX not found. Set ULTI_SIGN_PFX (or pass -PfxPath)."
}

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { throw "signtool.exe not found (install the Windows SDK signing tools)." }

$signed = 0
foreach ($f in $Files) {
    $full = Join-Path $Path $f
    if (-not (Test-Path $full)) {
        Write-Warning "skip (missing): $full"
        continue
    }
    & $signtool.FullName sign /fd SHA256 /f $PfxPath /p $PfxPassword /tr $TimestampUrl /td SHA256 /v $full
    if ($LASTEXITCODE -ne 0) { throw "signing failed: $full" }
    $signed++
}

Write-Host "Signed $signed file(s) in $Path"
Write-Host "The installer is signed by Inno Setup itself when built with /DSignToolName (see build.yml)."
