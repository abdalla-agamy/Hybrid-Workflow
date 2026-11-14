# Progress Log — DT-HSBP Hybrid Workflow

## Phase 1: Workspace Initialization & Directory Scaffolding (2026-11-14)
- **Built**: Created `./Hybrid-Workflow` sibling to read-only `./DT-HSBP` with complete directory tree (`matlab_layer/`, `traces/schemas/`, `traces/samples/`, `python_simpy_layer/`, `omnetpp_inet_layer/`, `baseline_matlab_results/`, `comparison_reports/`, `scripts/`, `docs/`). Copied baseline MATLAB sources, cached 90-run `.mat` dataset, baseline figures/reports, and mathematical model docs without copying `DT-HSBP/.git`. Configured `.gitignore`, `README.md`, `PLAN_CHANGELOG.md`.
- **Tested**: Ran `ExperimentSmokeTest` in `Hybrid-Workflow/matlab_layer/` (`matlab -batch ...`) — **PASS** (`SMOKE TEST PASSED`, 18 Join + 18 Leave observations across 6 groups each).
- **Commit**: `eb7d11b`

## Phase 2.1: JSON Schema Contracts & Baseline Metrics Extraction (2026-11-14)
- **Built**: Defined Draft-07 JSON Schemas `traces/schemas/swarm_trace_schema.json` (with bounded per-event `nodePositions` scoping) and `traces/schemas/matlab_metrics_schema.json` (covering Sections 1-6 of `runCompleteEvaluationAndReport.m`). Added `matlab_layer/exporters/exportBaselineMetrics.m` and exported `baseline_matlab_results/matlab_baseline_metrics.json`.
- **Tested**: Validated both schemas (`Draft7Validator.check_schema`) and validated `matlab_baseline_metrics.json` against `matlab_metrics_schema.json` using Python `jsonschema` — **PASS**.
- **Commit**: `c5aedc5`

## Phase 2.2: TraceExporter & Simulation Instrumentation (2026-11-14)
- **Built**: Implemented `matlab_layer/exporters/TraceExporter.m` with continuous specular boundary reflection (`projectPosition`), full initial-swarm snapshotting, bounded per-event `nodePositions` scoping (only SL, target CH, and post-event cluster followers), protocol-specific multi-hop wireless frame generation (`FlatSBP`, `HSBP`, `DTHSBP`, `LKH`, `TGDH`, plus `FOLLOWER_TO_CH_DT`), and UTF-8 JSON export. Instrumented `matlab_layer/simulation/Simulation.m` gated strictly behind `cfg.enableTraceExport` (with paired per-`(t, uavId)` DT physical instability scheduling across all protocols and explicit triggering `clusterID` propagation). Documented baseline adaptations in `PLAN_CHANGELOG.md`.
- **Tested**: Ran `ExperimentSmokeTest` (`SMOKE TEST PASSED`, 18 Join + 18 Leave observations) for zero regression, and exported/validated an `N=50` `DTHSBP` trace (27 events, 50 initial nodes) against `swarm_trace_schema.json` — **PASS**.
- **Commit**: `9c05a45`

## Phase 2.3: LKH/TGDH Trace Adapter & Multi-Scale Trace Export (2026-11-14)
- **Built**: Implemented `matlab_layer/exporters/TreeProtocolTraceAdapter.m` (wrapping `LKH(swarm, 3)` and `TGDH(swarm, 2)` with RNG state restoration preserving exogenous event and node-spawn parity) and `matlab_layer/experiments/runHybridTraceExport.m`. Updated `.gitignore` (`traces/runs/`) and `PLAN_CHANGELOG.md`.
- **Tested**: Executed `runHybridTraceExport()` across all 5 protocols (`FlatSBP`, `HSBP`, `DTHSBP`, `LKH`, `TGDH`) at `N in {50, 100, 200, 250, 1000}` (`25` traces generated in `traces/runs/`, plus `5` sample fixtures in `traces/samples/events_trace_<proto>_N50.json`). Observed paired event parity (`105` events on reactive protocols vs `100-101` on batched `DTHSBP`) — **PASS**.
