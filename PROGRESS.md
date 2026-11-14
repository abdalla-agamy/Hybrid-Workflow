# Progress Log — DT-HSBP Hybrid Workflow

## Phase 1: Workspace Initialization & Directory Scaffolding (2026-11-14)
- **Built**: Created `./Hybrid-Workflow` sibling to read-only `./DT-HSBP` with complete directory tree (`matlab_layer/`, `traces/schemas/`, `traces/samples/`, `python_simpy_layer/`, `omnetpp_inet_layer/`, `baseline_matlab_results/`, `comparison_reports/`, `scripts/`, `docs/`). Copied baseline MATLAB sources, cached 90-run `.mat` dataset, baseline figures/reports, and mathematical model docs without copying `DT-HSBP/.git`. Configured `.gitignore`, `README.md`, `PLAN_CHANGELOG.md`.
- **Tested**: Ran `ExperimentSmokeTest` in `Hybrid-Workflow/matlab_layer/` (`matlab -batch ...`) — **PASS** (`SMOKE TEST PASSED`, 18 Join + 18 Leave observations across 6 groups each).
