<#
.SYNOPSIS
    Native Windows 10 setup and build helper for Phase 5 OMNeT++ 6.x + INET 4.5 replay layer.
.DESCRIPTION
    Checks for OMNeT++ 6.x (opp_run.exe / opp_makemake) and INET 4.5 framework on Windows 10.
    If present (via -OmnetRoot / $env:OMNETPP_ROOT and -InetRoot / $env:INET_ROOT), configures the
    MinGW environment, runs opp_makemake with nlohmann/json include paths, compiles the
    dt_hsbp_omnet binary inside ./omnetpp_inet_layer, and executes the headless Cmdenv verification run.
    If absent, prints verified native Windows installation instructions (no WSL/Docker required).
#>
[CmdletBinding()]
param(
    [string]$OmnetRoot = $env:OMNETPP_ROOT,
    [string]$InetRoot = $env:INET_ROOT,
    [switch]$BuildAndTest
)

$ErrorActionPreference = "Stop"
$LayerDir = (Resolve-Path (Join-Path $PSScriptRoot "..\omnetpp_inet_layer")).Path

function Find-OmnetRoot {
    if ($OmnetRoot -and (Test-Path (Join-Path $OmnetRoot "bin\opp_run.exe"))) {
        return (Resolve-Path $OmnetRoot).Path
    }
    foreach ($drive in @("C:", "D:", "E:", "F:", "G:", "H:")) {
        $candidates = Get-ChildItem -Path "$drive\" -Filter "omnetpp-*" -Directory -ErrorAction SilentlyContinue
        foreach ($c in $candidates) {
            if (Test-Path (Join-Path $c.FullName "bin\opp_run.exe")) {
                return $c.FullName
            }
        }
    }
    return $null
}

function Find-InetRoot([string]$ResolvedOmnetRoot) {
    if ($InetRoot -and (Test-Path (Join-Path $InetRoot "src\INET\package.ned"))) {
        return (Resolve-Path $InetRoot).Path
    }
    if ($ResolvedOmnetRoot) {
        $sibling = Get-ChildItem -Path (Split-Path $ResolvedOmnetRoot -Parent) -Filter "inet*" -Directory -ErrorAction SilentlyContinue
        foreach ($c in $sibling) {
            if (Test-Path (Join-Path $c.FullName "src\INET\package.ned")) {
                return $c.FullName
            }
        }
        $samples = Join-Path $ResolvedOmnetRoot "samples\inet*"
        $nested = Get-Item $samples -ErrorAction SilentlyContinue | Where-Object { Test-Path (Join-Path $_.FullName "src\INET\package.ned") } | Select-Object -First 1
        if ($nested) { return $nested.FullName }
    }
    return $null
}

$FoundOmnet = Find-OmnetRoot
$FoundInet = Find-InetRoot $FoundOmnet

if (-not $FoundOmnet -or -not $FoundInet) {
    Write-Warning "Native Windows OMNeT++ 6.x / INET 4.5 installation not found."
    Write-Host ""
    Write-Host "Required components for full packet-level INET 802.11 simulation (Phase 5B):" -ForegroundColor Cyan
    Write-Host "  1. Download OMNeT++ 6.0.x or 6.1.x Windows zip from: https://omnetpp.org/download/"
    Write-Host "  2. Extract e.g. to C:\omnetpp-6.1 (keep path short and without spaces)."
    Write-Host "  3. Launch mingwenv.cmd once, then configure & build OMNeT++ and INET 4.5:"
    Write-Host "       ./configure && make -j8"
    Write-Host "       cd samples && opp_get_inet   # or unpack inet-4.5.x into C:\omnetpp-6.1\samples\inet4.5"
    Write-Host "       cd inet4.5 && make makefiles && make -j8"
    Write-Host "  4. Re-run this script in PowerShell:"
    Write-Host "       powershell -ExecutionPolicy Bypass -File scripts\setup_omnetpp_windows.ps1 -OmnetRoot C:\omnetpp-6.1 -InetRoot C:\omnetpp-6.1\samples\inet4.5 -BuildAndTest"
    Write-Host ""
    Write-Host "Note: Standalone C++ TraceEventPlayer parser/carry-forward tests compile right now via MinGW g++.exe with -DOMNETPP_STANDALONE_TEST." -ForegroundColor Yellow
    exit 1
}

Write-Host "[OK] OMNETPP_ROOT = $FoundOmnet" -ForegroundColor Green
Write-Host "[OK] INET_ROOT    = $FoundInet" -ForegroundColor Green

$env:OMNETPP_ROOT = $FoundOmnet
$env:INET_ROOT = $FoundInet
$MinGwBin = Join-Path $FoundOmnet "tools\win64\mingw64\bin"
$OmnetBin = Join-Path $FoundOmnet "bin"
$InetSrc = Join-Path $FoundInet "src"
$InetLib = Join-Path $FoundInet "out\clang-release\src"
$env:PATH = "$OmnetBin;$MinGwBin;$InetLib;$env:PATH"

if ($BuildAndTest) {
    Push-Location $LayerDir
    try {
        $Bash = Join-Path $FoundOmnet "tools\win64\usr\bin\bash.exe"
        if (-not (Test-Path $Bash)) {
            throw "OMNeT++ MSYS2 bash.exe not found at $Bash"
        }
        $MakeCmd = "cd '$($LayerDir -replace '\\','/')' && opp_makemake -f --deep -O out -KINET_PROJ='$($FoundInet -replace '\\','/')' -DINET_IMPORT -I`$`(INET_PROJ`)/src -L`$`(INET_PROJ`)/src -lINET`$`(D`) && make MODE=release -j4"
        & $Bash -lc $MakeCmd
        if ($LASTEXITCODE -ne 0) { throw "OMNeT++ build failed with exit code $LASTEXITCODE" }

        $NedPath = "$($LayerDir)\simulations;$($LayerDir)\src;$($InetSrc)"
        & (Join-Path $LayerDir "out\clang-release\dt_hsbp_omnet.exe") -u Cmdenv -n $NedPath -c DTHSBP_Smoke "$LayerDir\simulations\omnetpp.ini"
        if ($LASTEXITCODE -ne 0) { throw "OMNeT++ Cmdenv smoke run failed" }
    }
    finally {
        Pop-Location
    }
}
