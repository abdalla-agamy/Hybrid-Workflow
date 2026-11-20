<#
.SYNOPSIS
    Native Windows 10 setup and build helper for Phase 5 OMNeT++ 6.5.0 + INET 4.7.0 replay layer.
.DESCRIPTION
    Detects native Windows OMNeT++ 6.x (defaulting to F:\omnetpp-6.5.0 when present) and
    INET 4.x (F:\omnetpp-6.5.0\samples\inet-4.7.0), configures the MSYS2 CLANG64 toolchain,
    generates Makefiles via opp_makemake against libINET.dll, builds dt_hsbp_omnet,
    and optionally runs the headless Cmdenv smoke test.
#>
[CmdletBinding()]
param(
    [string]$OmnetRoot = $env:OMNETPP_ROOT,
    [string]$InetRoot = $env:INET_ROOT,
    [switch]$BuildOnly,
    [switch]$BuildAndTest
)

$ErrorActionPreference = "Stop"
$LayerDir = (Resolve-Path (Join-Path $PSScriptRoot "..\omnetpp_inet_layer")).Path

function Find-OmnetRoot {
    if ($OmnetRoot -and (Test-Path (Join-Path $OmnetRoot "bin\opp_run.exe"))) {
        return (Resolve-Path $OmnetRoot).Path
    }
    if (Test-Path "F:\omnetpp-6.5.0\bin\opp_run.exe") {
        return "F:\omnetpp-6.5.0"
    }
    foreach ($drive in @("F:", "C:", "D:", "E:", "G:", "H:")) {
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
    if ($InetRoot -and (Test-Path (Join-Path $InetRoot "src\inet\package.ned"))) {
        return (Resolve-Path $InetRoot).Path
    }
    if ($ResolvedOmnetRoot) {
        $samples = Join-Path $ResolvedOmnetRoot "samples\inet*"
        $nested = Get-Item $samples -ErrorAction SilentlyContinue | Where-Object {
            Test-Path (Join-Path $_.FullName "src\inet\package.ned")
        } | Select-Object -First 1
        if ($nested) { return $nested.FullName }

        $sibling = Get-ChildItem -Path (Split-Path $ResolvedOmnetRoot -Parent) -Filter "inet*" -Directory -ErrorAction SilentlyContinue
        foreach ($c in $sibling) {
            if (Test-Path (Join-Path $c.FullName "src\inet\package.ned")) {
                return $c.FullName
            }
        }
    }
    return $null
}

function Convert-ToMsysPath([string]$WinPath) {
    $norm = ($WinPath -replace "\\", "/")
    if ($norm -match "^([A-Za-z]):/(.*)$") {
        return "/" + $matches[1].ToLower() + "/" + $matches[2]
    }
    return $norm
}

$FoundOmnet = Find-OmnetRoot
$FoundInet = Find-InetRoot $FoundOmnet

if (-not $FoundOmnet -or -not $FoundInet) {
    Write-Warning "Native Windows OMNeT++ 6.x / INET 4.x installation not found."
    Write-Host ""
    Write-Host "Required components for full packet-level INET 802.11 simulation:" -ForegroundColor Cyan
    Write-Host "  1. Install OMNeT++ 6.5.0+ and INET 4.7.0+ (e.g. F:\omnetpp-6.5.0 and F:\omnetpp-6.5.0\samples\inet-4.7.0)."
    Write-Host "  2. Launch opp_shell.cmd (CLANG64) once and verify `opp_run -v` and INET build."
    Write-Host "  3. Re-run this script:"
    Write-Host "       powershell -ExecutionPolicy Bypass -File scripts\setup_omnetpp_windows.ps1 -BuildAndTest"
    exit 1
}

Write-Host "[OK] OMNETPP_ROOT = $FoundOmnet" -ForegroundColor Green
Write-Host "[OK] INET_ROOT    = $FoundInet" -ForegroundColor Green

$env:OMNETPP_ROOT = $FoundOmnet
$env:INET_ROOT = $FoundInet

$MsysBash = Join-Path $FoundOmnet "tools\win32.x86_64\usr\bin\bash.exe"
if (-not (Test-Path $MsysBash)) {
    throw "OMNeT++ MSYS2 bash.exe not found at $MsysBash"
}

$OmnetMsys = Convert-ToMsysPath $FoundOmnet
$InetMsys = Convert-ToMsysPath $FoundInet
$LayerMsys = Convert-ToMsysPath $LayerDir

if ($BuildOnly -or $BuildAndTest) {
    $env:MSYSTEM = "CLANG64"
    $env:CHERE_INVOKING = "1"
    $BuildCmd = "source '$OmnetMsys/setenv' && cd '$LayerMsys' && opp_makemake -f --deep -e cc -X tests -O out -o dt_hsbp_omnet -Isrc -Isrc/third_party -KINET_PROJ='$InetMsys' -DINET_IMPORT -I'`$(INET_PROJ)/src' -L'`$(INET_PROJ)/src' -lINET'`$(D)' && make MODE=release -j4"
    Write-Host "[Build] Running opp_makemake + make MODE=release inside MSYS2 CLANG64..." -ForegroundColor Cyan
    & $MsysBash -lc $BuildCmd
    if ($LASTEXITCODE -ne 0) {
        throw "OMNeT++ build failed with exit code $LASTEXITCODE"
    }
}

if ($BuildAndTest) {
    $ExePath = if (Test-Path (Join-Path $LayerDir "dt_hsbp_omnet.exe")) {
        (Join-Path $LayerDir "dt_hsbp_omnet.exe")
    } elseif (Test-Path (Join-Path $LayerDir "out\clang-release\dt_hsbp_omnet.exe")) {
        (Join-Path $LayerDir "out\clang-release\dt_hsbp_omnet.exe")
    } else {
        (Join-Path $LayerDir "out\clang-release\src\dt_hsbp_omnet.exe")
    }
    $InetWinForward = ($FoundInet -replace "\\", "/")
    $NedArg = ".;../src;$InetWinForward/src"
    $SavedPath = $env:PATH
    try {
        $env:PATH = "$FoundOmnet\bin;$FoundOmnet\tools\win32.x86_64\clang64\bin;$FoundInet\src;$env:PATH"
        Push-Location (Join-Path $LayerDir "simulations")
        Write-Host "[Test] Running headless Cmdenv DTHSBP_Smoke verification ($ExePath)..." -ForegroundColor Cyan
        & $ExePath -u Cmdenv -n $NedArg -c DTHSBP_Smoke omnetpp.ini
        if ($LASTEXITCODE -ne 0) {
            throw "OMNeT++ Cmdenv DTHSBP_Smoke run failed with exit code $LASTEXITCODE"
        }
        Write-Host "[PASS] DTHSBP_Smoke completed cleanly." -ForegroundColor Green
    }
    finally {
        Pop-Location
        $env:PATH = $SavedPath
    }
}
