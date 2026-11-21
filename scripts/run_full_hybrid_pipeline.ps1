<#
.SYNOPSIS
    End-to-end master pipeline orchestrator for the DT-HSBP Hybrid Evaluation Workflow (Windows 10).
.DESCRIPTION
    Orchestrates all 4 layers of ./Hybrid-Workflow strictly without modifying sibling ./DT-HSBP:
      1. Baseline Integrity Check & optional MATLAB trace/baseline regeneration (-ReRunMatlab)
      2. Draft-07 JSON Schema & Bounded NodePosition Validation (validate_traces.py)
      3. SimPy CSMA/CA DES Layer verification (& optional sweep regeneration via -ReRunSimPy)
      4. OMNeT++ 6.5.0 / INET 4.7.0 802.11g Layer (standalone C++17 parser test, optional sweep via -ReRunOmnet, pytest integration)
      5. Phase 4 (MATLAB vs. SimPy) & Phase 6 (Tri-Layer MATLAB vs. SimPy vs. OMNeT++) Report Generation
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\run_full_hybrid_pipeline.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\run_full_hybrid_pipeline.ps1 -ReRunMatlab -ReRunSimPy -ReRunOmnet -OmnetWorkers 4
#>
[CmdletBinding()]
param(
    [switch]$ReRunMatlab,
    [switch]$ReRunSimPy,
    [switch]$ReRunOmnet,
    [int]$OmnetWorkers = 4,
    [string]$OmnetRoot = $(if ($env:OMNETPP_ROOT) { $env:OMNETPP_ROOT } else { "F:\omnetpp-6.5.0" })
)

$ErrorActionPreference = "Stop"
$WorkflowRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BaselineDir = (Resolve-Path (Join-Path $WorkflowRoot "..\DT-HSBP")).Path
$PythonExe = Join-Path $WorkflowRoot "python_simpy_layer\.venv\Scripts\python.exe"

if (-not (Test-Path $PythonExe)) {
    throw "Python virtual environment not found at $PythonExe"
}

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][scriptblock]$Action
    )
    Write-Host ""
    Write-Host "========================================================================" -ForegroundColor Cyan
    Write-Host "  $Title" -ForegroundColor Cyan
    Write-Host "========================================================================" -ForegroundColor Cyan
    $global:LASTEXITCODE = 0
    & $Action
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        throw "Pipeline step failed with exit code ${LASTEXITCODE}: $Title"
    }
}

$BaselineLatestBefore = (Get-ChildItem -Path $BaselineDir -Recurse -File | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
Write-Host "[Init] WorkflowRoot : $WorkflowRoot"
Write-Host "[Init] BaselineDir  : $BaselineDir (read-only latest write: $($BaselineLatestBefore.ToString('o')))"

# 1. MATLAB Trace Exporter & Baseline Verification
Invoke-Step "Stage 1: MATLAB Baseline & Trace Export" {
    if ($ReRunMatlab) {
        $matlabDir = (Join-Path $WorkflowRoot "matlab_layer") -replace "'", "''"
        matlab -batch "cd('$matlabDir'); addpath(genpath('.')); runtests('tests/TestTraceExporter.m'); exportBaselineMetrics(); runHybridTraceExport();"
    } else {
        $required = @(
            (Join-Path $WorkflowRoot "baseline_matlab_results\matlab_baseline_metrics.json"),
            (Join-Path $WorkflowRoot "traces\samples\events_trace_DTHSBP_N50.json"),
            (Join-Path $WorkflowRoot "traces\runs\events_trace_DTHSBP_N250.json")
        )
        foreach ($f in $required) {
            if (-not (Test-Path $f)) {
                throw "Missing MATLAB artifact: $f (pass -ReRunMatlab to regenerate)"
            }
        }
        Write-Host "[OK] Verified existing MATLAB baseline JSON and deterministic trace fixtures."
    }
}

# 2. Trace JSON Schema Validation
Invoke-Step "Stage 2: JSON Schema & Bounded Coordinate Validation" {
    & $PythonExe (Join-Path $WorkflowRoot "scripts\validate_traces.py")
}

# 3. Python SimPy DES Replay Layer
Invoke-Step "Stage 3: Python SimPy 802.11 CSMA/CA Replay & Tests" {
    if ($ReRunSimPy) {
        & $PythonExe (Join-Path $WorkflowRoot "python_simpy_layer\src\experiment_runner.py")
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    Push-Location (Join-Path $WorkflowRoot "python_simpy_layer")
    try {
        & $PythonExe -m pytest tests/ -v
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    finally {
        Pop-Location
    }
}

# 4. OMNeT++ 6.5.0 / INET 4.7.0 Packet-Level Replay Layer
Invoke-Step "Stage 4: OMNeT++ / INET C++ Parser, Sweep & Integration Tests" {
    $ClangBin = Join-Path $OmnetRoot "tools\win32.x86_64\clang64\bin"
    $ClangExe = Join-Path $ClangBin "clang++.exe"
    if (Test-Path $ClangExe) {
        $ParserTestBin = Join-Path $WorkflowRoot "omnetpp_inet_layer\out\test_omnet_json_parser.exe"
        New-Item -ItemType Directory -Force -Path (Split-Path $ParserTestBin -Parent) | Out-Null
        $SavedPath = $env:PATH
        $env:PATH = "$ClangBin;$env:PATH"
        try {
            & $ClangExe -std=c++17 -O2 -DOMNETPP_STANDALONE_TEST `
                -I(Join-Path $WorkflowRoot "omnetpp_inet_layer\src") `
                -I(Join-Path $WorkflowRoot "omnetpp_inet_layer\src\third_party") `
                (Join-Path $WorkflowRoot "omnetpp_inet_layer\src\TraceEventPlayer.cc") `
                (Join-Path $WorkflowRoot "omnetpp_inet_layer\tests\test_omnet_json_parser.cpp") `
                -o $ParserTestBin
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
            Push-Location (Join-Path $WorkflowRoot "omnetpp_inet_layer")
            try {
                & $ParserTestBin
                if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
            }
            finally {
                Pop-Location
            }
        }
        finally {
            $env:PATH = $SavedPath
        }
    } else {
        Write-Warning "clang++.exe not found at $ClangExe; skipping standalone C++ parser build check."
    }

    if ($ReRunOmnet) {
        & powershell -ExecutionPolicy Bypass -File (Join-Path $WorkflowRoot "scripts\setup_omnetpp_windows.ps1") -BuildAndTest
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Remove-Item -Path (Join-Path $WorkflowRoot "omnetpp_inet_layer\results\*.sca"),(Join-Path $WorkflowRoot "omnetpp_inet_layer\results\*.vec"),(Join-Path $WorkflowRoot "omnetpp_inet_layer\results\*.vci"),(Join-Path $WorkflowRoot "omnetpp_inet_layer\results\omnet_events_*.csv") -ErrorAction SilentlyContinue
        & $PythonExe (Join-Path $WorkflowRoot "omnetpp_inet_layer\src\run_omnet_sweep.py") --configs DynamicStochastic Scalability LossResilience --workers $OmnetWorkers
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & $PythonExe (Join-Path $WorkflowRoot "omnetpp_inet_layer\src\parse_omnet_scalars.py")
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    & $PythonExe -m pytest (Join-Path $WorkflowRoot "omnetpp_inet_layer\tests") -v
}

# 5. Phase 4 & Phase 6 Cross-Layer Reports + Baseline Integrity Audit
Invoke-Step "Stage 5: Tri-Layer Report Generation & Baseline Integrity Audit" {
    & $PythonExe (Join-Path $WorkflowRoot "comparison_reports\phase4_matlab_vs_simpy\generate_phase4_report.py")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $PythonExe (Join-Path $WorkflowRoot "comparison_reports\phase6_tri_layer_benchmark\generate_tri_layer_report.py")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $BaselineLatestAfter = (Get-ChildItem -Path $BaselineDir -Recurse -File | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
    if ($BaselineLatestAfter -ne $BaselineLatestBefore) {
        throw "Read-only violation detected: ./DT-HSBP was modified during pipeline execution!"
    }
    Write-Host "[OK] Confirmed ./DT-HSBP remained 100% untouched." -ForegroundColor Green
}

Write-Host ""
Write-Host "[SUCCESS] Full Hybrid Evaluation Workflow completed with 0 failures." -ForegroundColor Green
