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
- **Commit**: `2734c47`

## Phase 2.4: Trace Exporter Tests, Schema Validation & Zero-Regression Gate (2026-11-14)
- **Built**: Created `matlab_layer/tests/TestTraceExporter.m` (`5` unit/property tests covering determinism, boundary projection, membership/collateral-batch transitions, 5-protocol byte/hop semantics, and bounded `nodePositions` scoping at `N=250`) and `scripts/validate_traces.py`.
- **Tested**:
  - `runtests('tests/TestTraceExporter.m')`: **5/5 PASS**.
  - `python scripts/validate_traces.py`: **30/30** exported trace JSONs + `matlab_baseline_metrics.json` schema-valid & bounded-scoping PASS.
  - Zero-regression checks: `ExperimentSmokeTest` (**PASS**, 18/18 join, 18/18 leave), `ComparativeStochasticStudyTest` (**PASS**, 12/12 runs), and bit-for-bit `runStochasticExperiment` parity verified against untouched `./DT-HSBP` baseline (pre-existing `FullStochasticMetricsTest` `hsbpNoDTInvariant=0` discrepancy logged in `PLAN_CHANGELOG.md`).
- **Commit**: `59fc765`

## Phase 3A: Python Trace Loader, Carry-Forward Topology & Wireless Channel (2026-11-14)
- **Built**: Created `python_simpy_layer/requirements.txt`, `--system-site-packages` `.venv` with `simpy 4.1.2` + `pytest 9.2.0`, `python_simpy_layer/src/{__init__.py, trace_loader.py, topology_manager.py, wireless_channel.py}`, and `docs/architecture.md`. Implemented bounded `nodePositions` carry-forward with dynamic CH/SL reassignment, NetworkX range graph builder, and log-distance path loss + shadowing + Gilbert-Elliott bursty PER + `d/3e8` propagation delay.
- **Tested**: Ran `python -m pytest tests/test_part3a_channel_topology.py -v` — **5/5 PASS**.
- **Commit**: `b8b283a`

## Phase 3B: CSMA/CA MAC, MTU Fragmentation & 5-Protocol Replay Agents (2026-11-14)
- **Built**: Implemented `python_simpy_layer/src/radio_mac.py` (spatial CSMA/CA carrier sensing inside `700m`, binary exponential backoff `CW_min=15..CW_max=1023`, `1500B` MTU fragmentation, unicast ACK retries & vectorized broadcast re-transmission rounds) and `python_simpy_layer/src/protocol_replay_agents.py` (replaying `FlatSBP`, `HSBP`, `DTHSBP`, `LKH`, `TGDH`, and `FOLLOWER_TO_CH_DT` over vectorized BFS relays with full per-event & summary metrics). Updated `topology_manager.py` with `scipy.sparse.csgraph` `compute_bfs_paths`.
- **Tested**: Ran `python -m pytest tests/test_part3a_channel_topology.py tests/test_part3b_mac_protocols.py -v` — **10/10 PASS**.
- **Commit**: `056036c`

## Phase 3C: YAML-Driven Experiment Runner & Full 200-Run Network Sweep (2026-11-14)
- **Built**: Created `python_simpy_layer/configs/simpy_experiments.yaml` (`seed=42`, `comm_range_m=350.0`, `carrier_sense_range_m=700.0`, `mtu_bytes=1500`, `max_retries=3`, `p_loss=[0.0,0.05,0.10,0.15]`, `data_rate_mbps=[6.0,54.0]`, `protocols=[FlatSBP,HSBP,DTHSBP,LKH,TGDH]`, `swarm_sizes=[50,100,200,250,1000]`) and parallelized `python_simpy_layer/src/experiment_runner.py` with `ProcessPoolExecutor`. Added `scipy.spatial.cKDTree` neighbor acceleration (`topology_manager.py`, `radio_mac.py`), sequential delivery sampling (`wireless_channel.py`), and `_disseminate_cluster_broadcast` depth-1 cross-cluster relay + greedy set cover (`protocol_replay_agents.py`), raising `p_loss=0.0` `mean_key_sync_ratio` to `0.901–0.921` (`N=50`) and `0.998–1.000` (`N>=100`).
- **Tested**: Ran `pytest tests/test_part3a_channel_topology.py tests/test_part3b_mac_protocols.py tests/test_part3c_runner.py -v` (**11/11 PASS**, `10.14s`) and executed full CLI sweep `python -m src.experiment_runner --config configs/simpy_experiments.yaml` (**200 runs**, **20824 events**, `217.24s`), generating `results/simpy_sweep_results.csv`, `results/simpy_summary_metrics.json`, and `results/simpy_per_event_metrics.csv`.
- **Commit**: `f8ebbc3`

## Phase 3D: End-to-End Integration Verification Across All 5 Protocol Traces (2026-11-14)
- **Built**: Created `python_simpy_layer/tests/test_part3d_integration.py` verifying committed `N=50` and `N=250` real MATLAB trace replays (`FlatSBP`, `HSBP`, `DTHSBP`, `LKH`, `TGDH`), full 200-run/20824-event sweep consistency, monotonic loss degradation, zero-loss key-sync bounds (`>= 0.90` at `N=50`, `>= 0.995` at `N>=100`), `DTHSBP` superiority over `FlatSBP`/`HSBP`, and the grounded rate-dependent `LKH`/`TGDH` vs `DTHSBP` latency crossover. Logged topology and CSMA/CA findings in `PLAN_CHANGELOG.md`.
- **Tested**: Ran `python -m pytest tests/ -v` — **20/20 PASS** (`24.82s`, zero skipped).
- **Commit**: `e32a9cb`

## Phase 4: Comparative MATLAB-vs-SimPy Report & 300-DPI Figures (2026-11-14)
- **Built**: Created `comparison_reports/phase4_matlab_vs_simpy/generate_phase4_report.py` loading metrics dynamically from `baseline_matlab_results/matlab_baseline_metrics.json` and the 200-run/20,824-event SimPy dataset (`simpy_sweep_results.csv`, `simpy_summary_metrics.json`, `simpy_per_event_metrics.csv`). Generated four 300-DPI publication PNGs (`fig1_communication_cost_matlab_vs_simpy.png`, `fig2_rekey_latency_breakdown.png`, `fig3_pdr_and_keysync_vs_loss.png`, `fig4_dt_batch_efficiency_under_loss.png`), `Phase4_MATLAB_vs_SimPy_Report.md`, and ReportLab `NumberedCanvas` `Phase4_MATLAB_vs_SimPy_Report.pdf`.
- **Tested**: Executed `python generate_phase4_report.py` (**exit 0**) and verified all 4 PNGs (`287–434 KB`), Markdown (`10.6 KB`), PDF (`1.63 MB`), and clean figure layout via visual inspection.
- **Commit**: `ea87900`

## Phase 5 Part 5A: OMNeT++/INET Environment Check, Scaffold & C++14 Trace Player (2026-11-14)
- **Built**: Verified `OMNETPP_ROOT`/`INET_ROOT`/`opp_run.exe` absent on `C:`–`H:` and logged the runtime gap plus MinGW-w64 GCC `5.4.0` C++14 adaptation in `PLAN_CHANGELOG.md`. Created `scripts/setup_omnetpp_windows.ps1` and scaffolded `omnetpp_inet_layer/` (`package.ned`, `simulations/package.ned`, `simulations/SwarmTraceNetwork.ned`, `simulations/omnetpp.ini`, `Makefile`, `src/package.ned`, `src/TraceEventPlayer.ned`, self-contained C++14 JSON parser `src/third_party/mini_json.hpp`, `src/TraceEventPlayer.{h,cc}`, and standalone unit test `tests/test_omnet_json_parser.cpp`).
- **Tested**: Compiled with `g++.exe -std=c++14 -O2 -Wall -Wextra -DOMNETPP_STANDALONE_TEST` (0 warnings, `CompileExit=0`) and ran `test_omnet_json_parser.exe` across all 5 `N=50` MATLAB sample traces (`FlatSBP`, `HSBP`, `DTHSBP`, `LKH`, `TGDH`) — **5/5 PASS** (`RunExit=0`, verified bounded snapshots `<= 16`, carry-forward across all clusters, CH/SL invariants, exact MATLAB `cryptoBytes`, and `DTHSBP` `collateralBatched=4`). Removed temporary `.exe` after testing.
- **Status**: **STOPPED at Part 5A gate** pending native Windows OMNeT++ 6.x + INET 4.5 installation for Phase 5B/Phase 6 packet-level execution.
- **Commit**: `79a1563`

## Phase 5 Part 5B: OMNeT++ 6.5.0 / INET 4.7.0 802.11 Topology, Bounded Mobility & Rekey App (2026-11-20)
- **Built**: Resumed upon user installation of native Windows **OMNeT++ 6.5.0** and **INET 4.7.0** at `F:\omnetpp-6.5.0`. Created `TraceMobility.{ned,h,cc}` (updating INET `StationaryMobility::lastPosition` strictly for bounded `nodePositions` snapshots), `DTHSBPPayload.h` (`WireRekeyHeader` 28B), `DTHSBPRekeyApp.{ned,h,cc}` (UDP/IPv4/802.11 fragment TX/RX over `wlan0` with `InterfaceReq`), `InetWinCompat.cc` (`inet::internal` Windows DLL linkage shim documented in `PLAN_CHANGELOG.md`), `simulations/DTHSBPSwarmNetwork.ned` (`UAVNode extends AdhocHost` + `Ieee80211ScalarRadioMedium` calibrated via `FreeSpacePathLoss` for `350m`/`700m`), updated `TraceEventPlayer.{ned,h,cc}`, `simulations/omnetpp.ini`, and `scripts/setup_omnetpp_windows.ps1`.
- **Tested**:
  - Python gate `pytest tests/ -v`: **20/20 PASS**.
  - Standalone C++17 `clang++.exe` `test_omnet_json_parser`: **5/5 PASS**.
  - Built `dt_hsbp_omnet.exe` via `opp_makemake` + `CLANG64` `make` (**0 errors**) and ran headless Cmdenv `DTHSBP_Smoke` (`132,749` events in `1.97 s` -> `meanPdr=0.9045`, `meanKeySyncRatio=0.8915`, `meanNetworkLatencyMs=8.109 ms`, `collateralLeavesBatched=4`).
- **Phase 5C (`omnetpp_inet_layer/simulations/omnetpp.ini`, `TraceEventPlayer.cc`, `parse_omnet_scalars.py`, `run_omnet_sweep.py`)**:
  - Defined headless `Scalability` (40 runs), `LossResilience` (80 runs), and `DynamicStochastic` (5 per-event CSV runs) configs in `omnetpp.ini`; enabled `MapCommunicationCache`, `maxTransmissionDuration = 5ms`, and `radioModeFilter`/`listeningFilter = true` to avoid `std::bad_alloc` under dense $N=250$ compound hosts (`234 MiB` RSS vs OOM).
  - Calibrated `armStageTimer` in `TraceEventPlayer.cc` for multi-hop `FlatSBP` batch draining (`Scalability #30` `FlatSBP N=250, 6 Mbps, p_loss=0.05` verified end-to-end across `64,487,419` events in `t=105s` -> `meanPdr=0.99996`, `meanNetworkLatencyMs=577.394 ms`).
  - Implemented `parse_omnet_scalars.py` (exact 22-column SimPy CSV/JSON schema + per-event CSV merge) and `run_omnet_sweep.py` (LPT-ordered 3-worker execution with cross-config grid deduplication -> 100 unique runs and automatic `.vec`/`.vci` cleanup).
- **Phase 5D (`omnetpp_inet_layer/src/SafeDcf.ned`, `InetWinCompat.cc`, `run_omnet_sweep.py`, `tests/test_part5d_omnet_integration.py`, `results/`)**:
  - Set `**.result-recording-modes = count` & `**.param-recording = false` (`19,442.7 MiB -> 127.7 MiB` virtual commit at $N=250$) and added `dthsbp::SafeDcf` to eliminate an upstream INET 4.7.0 `Dcf::processLowerFrame` `cancelEvent(startRxTimer)` ACK-window deadlock (`Scalability #35` queue overflows `27,931 -> 0`, restoring `105/105` events & `1.000` PDR).
  - Executed all **100** unique deduplicated OMNeT++ 6.5.0 / INET 4.7.0 sweeps (`DynamicStochastic` + `Scalability` + `LossResilience`, **0 failures**) and extracted `omnet_sweep_results.csv` (`100` rows x `22` cols), `omnet_summary_metrics.json`, and `omnet_per_event_metrics.csv` (`520` $N=250$ per-event records).
  - Integration suite `pytest omnetpp_inet_layer/tests/test_part5d_omnet_integration.py -v`: **4/4 PASS** (`0.22 s`).
- **Phase 6 (`comparison_reports/phase6_tri_layer_benchmark/`, `scripts/run_full_hybrid_pipeline.ps1`)**:
  - Created dynamic tri-layer report generator `generate_tri_layer_report.py` synthesizing `matlab_baseline_metrics.json`, `simpy_*` (`200` runs), and `omnet_*` (`100` runs) into 4 300-DPI PNG figures, `Phase6_TriLayer_Hybrid_Evaluation_Report.md`, and `Phase6_TriLayer_Hybrid_Evaluation_Report.pdf` (verified visually).
  - Created master Windows PowerShell pipeline orchestrator `scripts/run_full_hybrid_pipeline.ps1` with `./DT-HSBP` read-only baseline integrity enforcement across all validation, test, and reporting stages.
