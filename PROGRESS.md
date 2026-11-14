# Progress Log — DT-HSBP Hybrid Workflow

## Phase 1: Workspace Initialization & Directory Scaffolding (2026-11-14)
- **Built**: Created `./Hybrid-Workflow` sibling to read-only `./DT-HSBP` with complete directory tree (`matlab_layer/`, `traces/schemas/`, `traces/samples/`, `python_simpy_layer/`, `omnetpp_inet_layer/`, `baseline_matlab_results/`, `comparison_reports/`, `scripts/`, `docs/`). Copied baseline MATLAB sources, cached 90-run `.mat` dataset, baseline figures/reports, and mathematical model docs without copying `DT-HSBP/.git`. Configured `.gitignore`, `README.md`, `PLAN_CHANGELOG.md`.
- **Tested**: Ran `ExperimentSmokeTest` in `Hybrid-Workflow/matlab_layer/` (`matlab -batch ...`) — **PASS** (`SMOKE TEST PASSED`, 18 Join + 18 Leave observations across 6 groups each).
- **Commit**: `eb7d11b`

## Phase 2.1: JSON Schema Contracts & Baseline Metrics Extraction (2026-11-14)
- **Built**: Defined Draft-07 JSON Schemas `traces/schemas/swarm_trace_schema.json` (with bounded per-event `nodePositions` scoping) and `traces/schemas/matlab_metrics_schema.json` (covering Sections 1-6 of `runCompleteEvaluationAndReport.m`). Added `matlab_layer/exporters/exportBaselineMetrics.m` and exported `baseline_matlab_results/matlab_baseline_metrics.json`.
- **Tested**: Validated both schemas (`Draft7Validator.check_schema`) and validated `matlab_baseline_metrics.json` against `matlab_metrics_schema.json` using Python `jsonschema` — **PASS**.
