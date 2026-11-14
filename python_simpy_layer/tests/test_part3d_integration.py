"""Phase 3D end-to-end integration tests across all 5 MATLAB protocol traces and 200-run sweep outputs."""

from __future__ import annotations

import csv
import json
from pathlib import Path

import pytest

from src.protocol_replay_agents import SimulationConfig, replay_trace
from src.trace_loader import load_trace

LAYER_ROOT = Path(__file__).resolve().parents[1]
WORKFLOW_ROOT = LAYER_ROOT.parent
SAMPLES_DIR = WORKFLOW_ROOT / "traces" / "samples"
RUNS_DIR = WORKFLOW_ROOT / "traces" / "runs"
RESULTS_DIR = LAYER_ROOT / "results"

ALL_PROTOCOLS = ("FlatSBP", "HSBP", "DTHSBP", "LKH", "TGDH")


@pytest.mark.parametrize("protocol", ALL_PROTOCOLS)
def test_replay_all_5_sample_traces_n50_valid(protocol: str) -> None:
    """Every committed N=50 MATLAB sample trace replays cleanly with exact latency accounting."""
    trace_path = SAMPLES_DIR / f"events_trace_{protocol}_N50.json"
    assert trace_path.exists(), f"Missing committed fixture {trace_path}"

    trace = load_trace(trace_path, validate=True)
    cfg = SimulationConfig(
        p_loss=0.05,
        data_rate_mbps=6.0,
        comm_range_m=350.0,
        carrier_sense_range_m=700.0,
        mtu_bytes=1500,
        max_retries=3,
        seed=42,
        enable_multihop_relay=True,
    )
    summary, events = replay_trace(trace, cfg)

    assert summary.protocol == protocol
    assert summary.swarm_size == 50
    assert summary.num_events == len(events) == len(trace.events)
    assert summary.total_crypto_bytes_matlab > 0
    assert summary.total_bytes_on_air >= summary.total_crypto_bytes_matlab
    assert summary.fragments_sent > 0
    assert summary.mean_rekey_completion_latency_ms == pytest.approx(
        summary.mean_crypto_latency_ms + summary.mean_network_latency_ms,
        rel=1e-6,
    )
    assert 0.0 <= summary.mean_pdr <= 1.0
    assert 0.0 <= summary.mean_key_sync_ratio <= 1.0

    if protocol == "DTHSBP":
        assert summary.collateral_leaves_batched > 0
        assert summary.collateral_batches > 0
    else:
        assert summary.collateral_leaves_batched == 0


def test_replay_n250_comparative_efficiency_if_runs_present() -> None:
    """Replay N=250 traces at 54 Mbps / p_loss=0.05 directly through SimPy."""
    trace_paths = {p: RUNS_DIR / f"events_trace_{p}_N250.json" for p in ALL_PROTOCOLS}
    if not all(p.exists() for p in trace_paths.values()):
        pytest.skip("Optional git-ignored traces/runs N=250 artifacts not present")

    cfg = SimulationConfig(p_loss=0.05, data_rate_mbps=54.0, seed=42)
    summaries = {}
    for proto, tpath in trace_paths.items():
        summary, _ = replay_trace(load_trace(tpath, validate=False), cfg)
        summaries[proto] = summary

    # DT-HSBP strictly beats FlatSBP and HSBP in air bytes and latency at N=250, 54 Mbps, p=0.05
    assert summaries["DTHSBP"].total_bytes_on_air < summaries["FlatSBP"].total_bytes_on_air
    assert summaries["DTHSBP"].total_bytes_on_air < summaries["HSBP"].total_bytes_on_air
    assert (
        summaries["DTHSBP"].mean_rekey_completion_latency_ms
        < summaries["FlatSBP"].mean_rekey_completion_latency_ms
    )
    assert (
        summaries["DTHSBP"].mean_rekey_completion_latency_ms
        < summaries["HSBP"].mean_rekey_completion_latency_ms
    )
    # Multi-round tree handshakes make LKH and TGDH slower than DTHSBP at 54 Mbps, p=0.05
    assert (
        summaries["LKH"].mean_network_latency_ms
        > summaries["DTHSBP"].mean_network_latency_ms
    )
    assert (
        summaries["TGDH"].mean_network_latency_ms
        > summaries["DTHSBP"].mean_network_latency_ms
    )


def _load_sweep_rows() -> dict[tuple[str, int, float, float], dict[str, float]]:
    csv_path = RESULTS_DIR / "simpy_sweep_results.csv"
    assert csv_path.exists(), f"Run experiment_runner first; missing {csv_path}"
    out: dict[tuple[str, int, float, float], dict[str, float]] = {}
    with csv_path.open(encoding="utf-8") as f:
        for row in csv.DictReader(f):
            key = (
                row["protocol"],
                int(row["swarm_size"]),
                round(float(row["p_loss"]), 2),
                round(float(row["data_rate_mbps"]), 1),
            )
            out[key] = {k: float(v) for k, v in row.items() if k != "protocol"}
    return out


def test_full_200_run_sweep_structural_integrity() -> None:
    """Verify full 200-run sweep CSV/JSON/per-event artifacts exist and match grid."""
    rows = _load_sweep_rows()
    assert len(rows) == 200

    summary_json = json.loads((RESULTS_DIR / "simpy_summary_metrics.json").read_text(encoding="utf-8"))
    assert summary_json["meta"]["total_runs"] == 200
    assert summary_json["meta"]["total_events"] == 20824
    assert len(summary_json["runs"]) == 200

    with (RESULTS_DIR / "simpy_per_event_metrics.csv").open(encoding="utf-8") as f:
        event_count = sum(1 for _ in f) - 1
    assert event_count == 20824


def test_sweep_dthsbp_gains_and_rate_dependent_crossover() -> None:
    """Verify DTHSBP gains across the full sweep plus grounded rate-dependent tree crossover."""
    rows = _load_sweep_rows()
    sizes = (50, 100, 200, 250, 1000)
    losses = (0.0, 0.05, 0.10, 0.15)
    rates = (6.0, 54.0)

    # 1. DTHSBP strictly dominates FlatSBP across ALL 40 grid points
    for n in sizes:
        for p in losses:
            for r in rates:
                dt = rows[("DTHSBP", n, p, r)]
                flat = rows[("FlatSBP", n, p, r)]
                assert dt["total_crypto_bytes_matlab"] < flat["total_crypto_bytes_matlab"]
                assert dt["total_bytes_on_air"] < flat["total_bytes_on_air"]
                assert dt["mean_rekey_completion_latency_ms"] < flat["mean_rekey_completion_latency_ms"]

    # 2. For dense swarms (N >= 100), DTHSBP beats HSBP in bytes on air across every point
    for n in (100, 200, 250, 1000):
        for p in losses:
            for r in rates:
                dt = rows[("DTHSBP", n, p, r)]
                hs = rows[("HSBP", n, p, r)]
                assert dt["total_crypto_bytes_matlab"] < hs["total_crypto_bytes_matlab"]
                assert dt["total_bytes_on_air"] < hs["total_bytes_on_air"]

    # 3. At 54 Mbps, CSMA/CA round overhead exceeds DT-HSBP's broadcast at N <= 100 (and TGDH up to N=250)
    for n in (50, 100):
        for p in losses:
            dt_lat = rows[("DTHSBP", n, p, 54.0)]["mean_network_latency_ms"]
            assert rows[("LKH", n, p, 54.0)]["mean_network_latency_ms"] > dt_lat
            assert rows[("TGDH", n, p, 54.0)]["mean_network_latency_ms"] > dt_lat
    for n in (50, 100, 200, 250):
        for p in losses:
            dt_lat = rows[("DTHSBP", n, p, 54.0)]["mean_network_latency_ms"]
            assert rows[("TGDH", n, p, 54.0)]["mean_network_latency_ms"] > dt_lat

    # 4. Grounded crossover at N=250/1000, 6 Mbps: DTHSBP's nc*56B cluster payload serialization > LKH
    assert (
        rows[("DTHSBP", 1000, 0.05, 6.0)]["mean_network_latency_ms"]
        > rows[("LKH", 1000, 0.05, 6.0)]["mean_network_latency_ms"]
    )


def test_loss_degradation_and_key_sync_coverage_bounds() -> None:
    """Loss must monotonically raise air traffic & latency while zero-loss sync meets connectivity bounds."""
    rows = _load_sweep_rows()
    for proto in ALL_PROTOCOLS:
        for n in (50, 100, 200, 250, 1000):
            for r in (6.0, 54.0):
                clean = rows[(proto, n, 0.0, r)]
                lossy = rows[(proto, n, 0.15, r)]
                assert lossy["total_bytes_on_air"] > clean["total_bytes_on_air"]
                assert (
                    lossy["mean_rekey_completion_latency_ms"]
                    > clean["mean_rekey_completion_latency_ms"]
                )
                assert lossy["mean_pdr"] < clean["mean_pdr"]
                assert lossy["mean_key_sync_ratio"] <= clean["mean_key_sync_ratio"]

                if n == 50:
                    assert clean["mean_key_sync_ratio"] >= 0.90
                else:
                    assert clean["mean_key_sync_ratio"] >= 0.995
