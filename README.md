# DT-HSBP Hybrid Cryptographic-Network Evaluation Pipeline

A multi-layer hybrid simulation workflow bridging the **Digital-Twin-Driven Hierarchical Scalable Batch Rekeying Protocol (DT-HSBP)** cryptographic/analytical MATLAB engine with packet-level wireless network simulation in Python (**SimPy + NetworkX**) and **OMNeT++ (INET Framework)**.

## Directory Structure

- `matlab_layer/`: Instrumented copy of `./DT-HSBP` (which remains strictly read-only), plus `exporters/` (`TraceExporter.m`, `TreeProtocolTraceAdapter.m`), `experiments/runHybridTraceExport.m`, and `tests/TestTraceExporter.m`.
- `traces/`:
  - `schemas/`: JSON Schema contracts (`swarm_trace_schema.json`, `matlab_metrics_schema.json`).
  - `samples/`: Small committed fixture traces (`N=50`) for unit and integration testing.
- `python_simpy_layer/`: Packet-level discrete-event wireless simulator (SimPy CSMA/CA MAC, NetworkX topology with bounded position carry-forward, log-distance PHY channel, multi-protocol replay, batch experiment sweeps, and pytest suite).
- `omnetpp_inet_layer/`: Native Windows OMNeT++ / INET project scaffold, C++ `TraceEventPlayer`, and standalone JSON parser tests.
- `baseline_matlab_results/`: Archived figures and evaluation reports from `./DT-HSBP` plus extracted JSON baseline metrics (`matlab_baseline_metrics.json`).
- `comparison_reports/`:
  - `phase4_matlab_vs_simpy/`: Comparative figures and report contrasting MATLAB analytical/crypto numbers with SimPy wireless packet replay.
  - `phase6_tri_layer_benchmark/`: Tri-layer benchmark figures and report (MATLAB vs SimPy vs OMNeT++/INET).
- `scripts/`: PowerShell/Python automation and schema validation helpers (`validate_traces.py`, `setup_omnetpp_windows.ps1`, etc.).
- `docs/`: Mathematical performance model reference and hybrid architecture documentation.
