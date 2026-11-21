"""Phase 5D integration tests for headless OMNeT++ 6.5.0 / INET 4.7.0 results and cross-simulator calibration."""

from __future__ import annotations

import csv
import json
import math
from pathlib import Path
from typing import Any

import pytest

LAYER_ROOT = Path(__file__).resolve().parents[1]
WORKFLOW_ROOT = LAYER_ROOT.parent
OMNET_RESULTS_DIR = LAYER_ROOT / "results"
SIMPY_RESULTS_DIR = WORKFLOW_ROOT / "python_simpy_layer" / "results"

OMNET_SWEEP_CSV = OMNET_RESULTS_DIR / "omnet_sweep_results.csv"
OMNET_SUMMARY_JSON = OMNET_RESULTS_DIR / "omnet_summary_metrics.json"
OMNET_EVENTS_CSV = OMNET_RESULTS_DIR / "omnet_per_event_metrics.csv"
SIMPY_SWEEP_CSV = SIMPY_RESULTS_DIR / "simpy_sweep_results.csv"

ALL_PROTOCOLS = ("FlatSBP", "HSBP", "DTHSBP", "LKH", "TGDH")
SWARM_SIZES = (50, 100, 200, 250)
DATA_RATES = (6.0, 54.0)
LOSS_LEVELS = (0.0, 0.05, 0.10, 0.15)


def _load_csv_rows(path: Path) -> tuple[list[str], dict[tuple[str, int, float, float], dict[str, Any]]]:
    assert path.exists(), f"Missing sweep CSV artifact: {path}"
    rows: dict[tuple[str, int, float, float], dict[str, Any]] = {}
    with path.open(encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        fieldnames = list(reader.fieldnames or [])
        for raw in reader:
            parsed: dict[str, Any] = {}
            for k, v in raw.items():
                if k == "protocol":
                    parsed[k] = v
                else:
                    val = float(v)
                    assert not math.isnan(val) and not math.isinf(val), f"Non-finite value for {k} in {raw}"
                    assert val >= 0.0, f"Negative metric {k}={val} in {raw}"
                    parsed[k] = val
            key = (
                parsed["protocol"],
                int(round(parsed["swarm_size"])),
                round(parsed["p_loss"], 4),
                round(parsed["data_rate_mbps"], 1),
            )
            rows[key] = parsed
    return fieldnames, rows


def test_omnet_sweep_schema_coverage_and_artifacts() -> None:
    """Verify 100-run deduplicated OMNeT++ CSV, JSON summary, and DynamicStochastic per-event log."""
    om_cols, om_rows = _load_csv_rows(OMNET_SWEEP_CSV)
    sp_cols, _ = _load_csv_rows(SIMPY_SWEEP_CSV)

    assert om_cols == sp_cols
    assert len(om_cols) == 22
    assert len(om_rows) == 100

    summary = json.loads(OMNET_SUMMARY_JSON.read_text(encoding="utf-8"))
    meta = summary["meta"]
    assert "OMNeT++" in meta["simulator"]
    assert "INET" in meta["simulator"]
    assert meta["total_runs"] == 100
    assert meta["sca_files_parsed"] == 100
    assert len(summary["runs"]) == 100
    assert meta["per_event_rows"] >= 500

    assert OMNET_EVENTS_CSV.exists()
    with OMNET_EVENTS_CSV.open(encoding="utf-8", newline="") as f:
        ev_rows = list(csv.DictReader(f))
    assert len(ev_rows) == meta["per_event_rows"]
    assert {r["protocol"] for r in ev_rows} == set(ALL_PROTOCOLS)
    assert all(int(r["swarm_size"]) == 250 for r in ev_rows)

    # Scalability grid: 4 sizes x 2 rates x 5 protocols @ p_loss=0.05 = 40 points
    for proto in ALL_PROTOCOLS:
        for n in SWARM_SIZES:
            for rate in DATA_RATES:
                assert (proto, n, 0.05, rate) in om_rows
        # LossResilience grid: {50, 250} x {6, 54 Mbps} x {0.0, 0.05, 0.10, 0.15}
        for n in (50, 250):
            for rate in DATA_RATES:
                for p_loss in LOSS_LEVELS:
                    assert (proto, n, p_loss, rate) in om_rows


def test_exact_trace_and_cryptographic_parity_with_simpy() -> None:
    """Every OMNeT++ scenario must replay the exact same MATLAB trace events and byte counts as SimPy."""
    _, om_rows = _load_csv_rows(OMNET_SWEEP_CSV)
    _, sp_rows = _load_csv_rows(SIMPY_SWEEP_CSV)

    parity_cols = (
        "num_events",
        "rekey_events",
        "dt_predicted_events",
        "collateral_batches",
        "collateral_leaves_batched",
        "total_crypto_bytes_matlab",
    )
    for key, om in om_rows.items():
        assert key in sp_rows, f"OMNeT++ key {key} missing from SimPy sweep"
        sp = sp_rows[key]
        for col in parity_cols:
            assert om[col] == sp[col], f"Trace parity mismatch on {key} col={col}: om={om[col]} vs sp={sp[col]}"
        assert 0.0 <= om["mean_pdr"] <= 1.0
        assert 0.0 <= om["mean_key_sync_ratio"] <= 1.0
        assert om["total_bytes_on_air"] >= om["total_crypto_bytes_matlab"]
        assert om["mean_rekey_completion_latency_ms"] == pytest.approx(
            om["mean_crypto_latency_ms"] + om["mean_network_latency_ms"],
            rel=1e-5,
            abs=1e-4,
        )


def test_zero_loss_cross_simulator_calibration_sanity() -> None:
    """At p_loss=0.0, INET 802.11g aligns with SimPy within calibrated physical SINR capture bounds."""
    _, om_rows = _load_csv_rows(OMNET_SWEEP_CSV)
    _, sp_rows = _load_csv_rows(SIMPY_SWEEP_CSV)

    for proto in ALL_PROTOCOLS:
        for n in (50, 250):
            # 6 Mbps BPSK/OFDM: tight air-byte & PDR agreement with SimPy CSMA/CA
            om6 = om_rows[(proto, n, 0.0, 6.0)]
            sp6 = sp_rows[(proto, n, 0.0, 6.0)]
            rel_bytes_6 = abs(om6["total_bytes_on_air"] - sp6["total_bytes_on_air"]) / sp6["total_bytes_on_air"]
            assert rel_bytes_6 <= 0.15, f"6 Mbps zero-loss bytes drift too high for {(proto, n)}: {rel_bytes_6:.3f}"
            assert abs(om6["mean_pdr"] - sp6["mean_pdr"]) <= 0.07

            # 54 Mbps 64-QAM 3/4: INET NistErrorRateModel captures hidden-terminal SNIR drops on dense N=250 bursts
            om54 = om_rows[(proto, n, 0.0, 54.0)]
            sp54 = sp_rows[(proto, n, 0.0, 54.0)]
            rel_bytes_54 = abs(om54["total_bytes_on_air"] - sp54["total_bytes_on_air"]) / sp54["total_bytes_on_air"]
            assert rel_bytes_54 <= 0.30, f"54 Mbps zero-loss bytes drift too high for {(proto, n)}: {rel_bytes_54:.3f}"
            assert abs(om54["mean_pdr"] - sp54["mean_pdr"]) <= 0.13


def test_omnet_protocol_ordering_and_loss_degradation() -> None:
    """Verify DT-HSBP advantage over FlatSBP/HSBP, 54 Mbps tree superiority, and monotonic loss impact."""
    _, om_rows = _load_csv_rows(OMNET_SWEEP_CSV)

    dt_points = [k[1:] for k in om_rows if k[0] == "DTHSBP"]
    assert len(dt_points) == 20

    # 1. DT-HSBP strictly outperforms FlatSBP across all 20 (swarm_size, p_loss, rate) points
    for n, p_loss, rate in dt_points:
        dt = om_rows[("DTHSBP", n, p_loss, rate)]
        flat = om_rows[("FlatSBP", n, p_loss, rate)]
        assert dt["total_bytes_on_air"] < flat["total_bytes_on_air"]
        assert dt["mean_rekey_completion_latency_ms"] < flat["mean_rekey_completion_latency_ms"]
        assert dt["fragments_sent"] < flat["fragments_sent"]
        assert dt["mac_collisions"] < flat["mac_collisions"]

    # 2. DT-HSBP beats HSBP in air bytes and fragments across all dense swarms (N>=100) and all 6 Mbps runs
    for n, p_loss, rate in dt_points:
        if n >= 100 or rate == 6.0:
            dt = om_rows[("DTHSBP", n, p_loss, rate)]
            hs = om_rows[("HSBP", n, p_loss, rate)]
            assert dt["total_bytes_on_air"] < hs["total_bytes_on_air"], f"Expected DTHSBP < HSBP at {(n, p_loss, rate)}"
            assert dt["fragments_sent"] < hs["fragments_sent"]
            assert dt["collateral_leaves_batched"] > 0
            assert hs["collateral_leaves_batched"] == 0

    # 3. At 54 Mbps (p_loss=0.05), INET tree contention makes DT-HSBP faster and more reliable than LKH/TGDH
    for n in SWARM_SIZES:
        dt = om_rows[("DTHSBP", n, 0.05, 54.0)]
        lkh = om_rows[("LKH", n, 0.05, 54.0)]
        tgdh = om_rows[("TGDH", n, 0.05, 54.0)]
        assert dt["mean_network_latency_ms"] < lkh["mean_network_latency_ms"]
        assert dt["mean_network_latency_ms"] < tgdh["mean_network_latency_ms"]
        assert dt["mean_key_sync_ratio"] > lkh["mean_key_sync_ratio"]
        assert dt["mean_key_sync_ratio"] > tgdh["mean_key_sync_ratio"]

    # 4. Loss degradation (p_loss=0.15 vs p_loss=0.0): traffic, retransmissions, and delay strictly increase
    for proto in ALL_PROTOCOLS:
        for n in (50, 250):
            for rate in DATA_RATES:
                clean = om_rows[(proto, n, 0.0, rate)]
                lossy = om_rows[(proto, n, 0.15, rate)]
                assert lossy["total_bytes_on_air"] > clean["total_bytes_on_air"]
                assert lossy["retransmissions"] > clean["retransmissions"]
                assert lossy["mean_rekey_completion_latency_ms"] > clean["mean_rekey_completion_latency_ms"]
