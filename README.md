# DT-HSBP Hybrid Cryptographic-Network Evaluation Pipeline

A multi-layer hybrid simulation workflow bridging the **Digital-Twin-Driven Hierarchical Scalable Batch Rekeying Protocol (DT-HSBP)** cryptographic/analytical MATLAB engine with packet-level wireless network simulation in **Python (SimPy + NetworkX)** and **OMNeT++ 6.5.0 / INET 4.7.0** (both Version 1 trace-driven tri-layer benchmarks and Version 2 closed-loop 54 Mbps IEEE 802.11e EDCA + physical air-to-air fading evaluation).

> **Note:** Sibling `../DT-HSBP` remains strictly read-only. Version 1 (`omnetpp_inet_layer/`) is frozen at Git tag `v1.0-trilayer-trace-replay`; Version 2 closed-loop work lives on branch `v2-edca-closed-loop` inside `omnetpp_inet_layer_v2/`.

## Directory Structure

- `matlab_layer/`: Instrumented copy of `../DT-HSBP`, plus `exporters/` (`TraceExporter.m`, `TreeProtocolTraceAdapter.m`), `experiments/runHybridTraceExport.m`, and `tests/TestTraceExporter.m`.
- `traces/`:
  - `schemas/`: JSON Schema contracts (`swarm_trace_schema.json`, `matlab_metrics_schema.json`).
  - `samples/`: Committed fixture traces (`N=50`) for unit and integration testing.
- `baseline_matlab_results/`: Archived figures and evaluation reports from `../DT-HSBP` plus extracted JSON baseline metrics (`matlab_baseline_metrics.json`).
- `python_simpy_layer/`: Packet-level SimPy 4 CSMA/CA discrete-event wireless simulator (`200`-run parametric sweep, NetworkX carry-forward topology, log-distance PHY channel, and `20`-test pytest suite).
- `omnetpp_inet_layer/` (**Version 1 — Frozen at `v1.0-trilayer-trace-replay`**): Native Windows OMNeT++ 6.5.0 / INET 4.7.0 IEEE 802.11g DCF trace-replay layer (`TraceEventPlayer` with `relayJitterMaxMs`, `SafeDcf`, `505`-run 5-seed CI sweep, and C++17 / pytest suites).
- `omnetpp_inet_layer_v2/` (**Version 2 — Closed-Loop EDCA + Fading + Native DT**): Native closed-loop protocol stack on branch `v2-edca-closed-loop` featuring:
  - `54 Mbps` IEEE 802.11e EDCA (`qosStation = true`, `Hcf`/`Edcaf`) with `UP = 6` (`AC_VO` rekey/ACK/resync), `UP = 4` (`AC_VI` `5 Hz` `128 B` encrypted C2), and `UP = 0` (`AC_BE` `1 Hz` `64 B` Heartbeat+DT reports & `5 s` CH summaries), plus `SafeDcf` (`qosStation = false`) ablation.
  - `RicianFading` ($K \in \{10, 4, 0\}\text{ dB}$, $\alpha = 2.0$, `2.412 GHz`, `100 mW`) with `Ieee80211NistErrorModel`.
  - Autonomous closed-loop agents (`DTKalmanTracker`, `LifespanChainOrder`, `KEY_RESYNC_PULL`, `REKEY_UNICAST_ACK`, SL failover, and counter-suppressed relay forwarding) across `170` unique runs / `43` 95% Student-t CI cells.
- `comparison_reports/`:
  - `phase4_matlab_vs_simpy/`: Phase 4 MATLAB-vs-SimPy figures and reports.
  - `phase6_tri_layer_benchmark/`: Version 1 tri-layer benchmark (MATLAB vs. SimPy vs. OMNeT++/INET DCF) 300-DPI figures and Markdown/PDF reports.
  - `v2_closed_loop_edca/`: Version 2 closed-loop 54 Mbps EDCA, Rician fading, security exposure, and EDCA-vs-DCF ablation figures (`v2_fig1..v2_fig4.png`) and Markdown/PDF reports.
- `scripts/`: Schema validation (`validate_traces.py`), Windows build automation (`setup_omnetpp_windows.ps1`, `build_v2_omnet.ps1`), and v1 master orchestrator (`run_full_hybrid_pipeline.ps1`).
- `docs/`: Mathematical performance model reference and hybrid architecture documentation.

## Version 2 Quickstart (Windows 10)

```powershell
# 1. Build dt_hsbp_omnet_v2.exe, run standalone C++ unit tests & V2_Smoke
powershell -ExecutionPolicy Bypass -File scripts/build_v2_omnet.ps1 -BuildAndTest

# 2. Run the 170-run multi-seed sweep (Scalability, FadingResilience, EDCA_vs_DCF_Ablation)
$py = "python_simpy_layer\.venv\Scripts\python.exe"
& $py omnetpp_inet_layer_v2\src\run_v2_sweep.py --workers 6

# 3. Aggregate Student-t 95% CI summaries & run V2 integration tests
& $py omnetpp_inet_layer_v2\src\parse_v2_scalars.py
& $py -m pytest omnetpp_inet_layer_v2\tests -v

# 4. Generate Version 2 report and 300-DPI figures
& $py comparison_reports\v2_closed_loop_edca\generate_v2_report.py
```

## Version 2 Headline Results (`N = 250`, `54 Mbps` EDCA, Mean ± 95% CI, `n = 5` Seeds)

| Condition | Protocol | DT Proactive Hit | Rekey Bytes (KB) | C2 Decrypt Rate | C2 Exposure (pkts) | Desync Outage (ms) |
|---|---|---:|---:|---:|---:|---:|
| Moderate Fading ($K = 4\text{ dB}$) | **DT-HSBP** | **39.8%** | **59.9 ± 7.9** | **99.92% ± 0.02%** | **3.84 ± 2.35** | **10.9 ± 19.4** |
| Moderate Fading ($K = 4\text{ dB}$) | HSBP | 0.0% | 165.0 ± 60.9 | 99.85% ± 0.29% | 23.75 ± 8.45 | 63.1 ± 130.0 |
| Moderate Fading ($K = 4\text{ dB}$) | Flat-SBP | 0.0% | 266.3 ± 73.3 | 83.63% ± 5.76% | 28.98 ± 7.73 | 563.7 ± 126.2 |
| Severe Rayleigh ($K = 0\text{ dB}$) | **DT-HSBP** | **39.8%** | **57.9 ± 7.9** | **99.87% ± 0.04%** | **3.84 ± 2.35** | **2.3 ± 1.7** |
| Severe Rayleigh ($K = 0\text{ dB}$) | HSBP | 0.0% | 175.1 ± 83.5 | 99.83% ± 0.29% | 23.75 ± 8.45 | 45.9 ± 114.8 |
| Severe Rayleigh ($K = 0\text{ dB}$) | Flat-SBP | 0.0% | 268.3 ± 68.0 | 81.16% ± 5.16% | 28.98 ± 7.73 | 627.9 ± 135.0 |
