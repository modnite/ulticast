# Release build of Ulticast with a bounded parallelism, so a compile can't
# monopolise every core of this machine (it also drives live video/calls).
# Override with:  $env:ULTICAST_JOBS = 8; .\build.ps1
$ErrorActionPreference = 'Stop'
$jobs = if ($env:ULTICAST_JOBS) { [int]$env:ULTICAST_JOBS } else { 4 }
Write-Host "Building Ulticast (Release) with $jobs parallel jobs..."
cmake --build (Join-Path $PSScriptRoot 'build') --config Release --parallel $jobs
