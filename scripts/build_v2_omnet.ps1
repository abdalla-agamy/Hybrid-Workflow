param(
    [string]$OmnetRoot = "F:\omnetpp-6.5.0",
    [string]$InetRoot = "F:\omnetpp-6.5.0\samples\inet-4.7.0",
    [switch]$BuildAndTest
)

$ErrorActionPreference = "Stop"

$LayerRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$LayerRoot = Join-Path $LayerRoot "omnetpp_inet_layer_v2"
$BashExe = Join-Path $OmnetRoot "tools\win32.x86_64\usr\bin\bash.exe"
if (-not (Test-Path $BashExe)) {
    throw "OMNeT++ MSYS2 bash not found at $BashExe"
}

function Convert-ToMsysPath([string]$WinPath) {
    $full = [System.IO.Path]::GetFullPath($WinPath)
    $drive = $full.Substring(0, 1).ToLowerInvariant()
    $rest = $full.Substring(2).Replace('\', '/')
    return "/$drive$rest"
}

$OmnetMsys = Convert-ToMsysPath $OmnetRoot
$InetMsys = Convert-ToMsysPath $InetRoot
$LayerMsys = Convert-ToMsysPath $LayerRoot

$env:MSYSTEM = "CLANG64"
$env:CHERE_INVOKING = "1"
$env:HOME = Join-Path $OmnetRoot "tools\win32.x86_64\home\$env:USERNAME"

Write-Host "[1/3] Generating Makefile and building dt_hsbp_omnet_v2.exe..."
$BuildCmd = "source '$OmnetMsys/setenv' && cd '$LayerMsys' && " +
    "opp_makemake -f --deep -e cc -X tests -O out -o dt_hsbp_omnet_v2 -Isrc " +
    "-KINET_PROJ='$InetMsys' -DINET_IMPORT -I'`$(INET_PROJ)/src' -L'`$(INET_PROJ)/src' -lINET'`$(D)' && " +
    "make MODE=release -j4"

& $BashExe -lc $BuildCmd
if ($LASTEXITCODE -ne 0) {
    throw "OMNeT++ v2 build failed with exit code $LASTEXITCODE"
}

Write-Host "[2/3] Building standalone C++ unit tests (test_v2_dt_logic.exe)..."
$TestCmd = "source '$OmnetMsys/setenv' && cd '$LayerMsys' && mkdir -p out && " +
    "clang++ -std=c++17 -O2 -Wall -Wextra -Isrc tests/test_v2_dt_logic.cpp -o out/test_v2_dt_logic.exe"

& $BashExe -lc $TestCmd
if ($LASTEXITCODE -ne 0) {
    throw "Standalone v2 C++ unit test build failed with exit code $LASTEXITCODE"
}

if ($BuildAndTest) {
    Write-Host "[3/3] Running test_v2_dt_logic.exe and V2_Smoke..."
    $SmokeCmd = "source '$OmnetMsys/setenv' && export PATH='$InetMsys/src':`"`$PATH`" && cd '$LayerMsys' && ./out/test_v2_dt_logic.exe && " +
        "cd simulations && ../dt_hsbp_omnet_v2.exe -u Cmdenv -c V2_Smoke -r 0 omnetpp.ini"
    & $BashExe -lc $SmokeCmd
    if ($LASTEXITCODE -ne 0) {
        throw "v2 unit test or V2_Smoke failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Build completed successfully."
