"""Integration tests verifying closed-loop v2 OMNeT++ EDCA, fading, DTServer & security outputs."""

from __future__ import annotations

import csv
import math
from pathlib import Path

import pytest

LAYER_ROOT = Path(__file__).resolve().parents[1]
RESULTS_DIR = LAYER_ROOT / "results"
SUMMARY_CSV = RESULTS_DIR / "v2_summary_ci95.csv"
RAW_CSV = RESULTS_DIR / "v2_raw_runs.csv"


def _load_summary() -> list[dict[str, str]]:
    if not SUMMARY_CSV.exists():
        pytest.skip(f"{SUMMARY_CSV} not generated yet")
    with SUMMARY_CSV.open("r", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def _load_raw() -> list[dict[str, str]]:
    if not RAW_CSV.exists():
        pytest.skip(f"{RAW_CSV} not generated yet")
    with RAW_CSV.open("r", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def _find_row(
    rows: list[dict[str, str]],
    config: str,
    protocol: str,
    swarm_size: int,
    rician_k_db: float,
    qos_enabled: int,
) -> dict[str, str]:
    for r in rows:
        if (
            r["config"] == config
            and r["protocol"] == protocol
            and int(r["swarm_size"]) == swarm_size
            and abs(float(r["rician_k_db"]) - rician_k_db) < 1e-3
            and int(r["qos_enabled"]) == qos_enabled
        ):
            return r
    raise AssertionError(
        f"Missing row config={config}, protocol={protocol}, N={swarm_size}, k={rician_k_db}, qos={qos_enabled}"
    )


def test_sweep_completeness_and_no_queue_overflows() -> None:
    summary = _load_summary()
    raw = _load_raw()

    # 20 scalability + 15 fading + 8 ablation = 43 aggregated parameter cells; 170 unique physical runs (shared k=4, qos=1)
    assert len(summary) == 43
    assert len(raw) == 170

    for r in summary:
        assert int(r["repetitions"]) == 5
        assert float(r["mac_queue_overflow_drops"]) == 0.0
        for col in [
            "rekey_bytes_on_air",
            "control_dt_bytes_on_air",
            "c2_bytes_on_air",
            "c2_decryption_success_rate",
            "mean_rekey_completion_latency_ms",
            "mean_exposure_window_packets",
        ]:
            val = float(r[col])
            assert not math.isnan(val)
            assert val >= 0.0


def test_edca_priorities_and_dcf_ablation_active() -> None:
    summary = _load_summary()
    for r in summary:
        if int(r["qos_enabled"]) == 1:
            assert float(r["edca_ac_be_tx"]) > 0.0
            assert float(r["edca_ac_vi_tx"]) > 0.0
            assert float(r["edca_ac_vo_tx"]) > 0.0
            assert float(r["dcf_tx"]) == 0.0
        else:
            assert float(r["dcf_tx"]) > 0.0
            assert float(r["edca_ac_vo_tx"]) == 0.0


def test_dthsbp_outperforms_reactive_baselines_at_n250() -> None:
    summary = _load_summary()
    dt = _find_row(summary, "V2_Scalability_54M", "DTHSBP", 250, 4.0, 1)
    hsbp = _find_row(summary, "V2_Scalability_54M", "HSBP", 250, 4.0, 1)
    flat = _find_row(summary, "V2_Scalability_54M", "FlatSBP", 250, 4.0, 1)

    assert float(dt["dt_prediction_hit_rate"]) > 0.30
    assert float(hsbp["dt_prediction_hit_rate"]) == 0.0
    assert float(flat["dt_prediction_hit_rate"]) == 0.0

    # Proactive DTServer exclusion dramatically slashes exposure window
    assert float(dt["mean_exposure_window_packets"]) < float(hsbp["mean_exposure_window_packets"])
    assert float(dt["mean_exposure_window_packets"]) < float(flat["mean_exposure_window_packets"])

    # Lifespan sorting O(1) tail truncation slashes rekey bytes vs HSBP and FlatSBP
    assert float(dt["rekey_bytes_on_air"]) < float(hsbp["rekey_bytes_on_air"])
    assert float(dt["rekey_bytes_on_air"]) < float(flat["rekey_bytes_on_air"])

    # C2 delivery stays strong (>= 85%)
    assert float(dt["c2_decryption_success_rate"]) >= 0.85


def test_rician_fading_impact_k10_vs_k0() -> None:
    summary = _load_summary()
    dt_k10 = _find_row(summary, "V2_FadingResilience_54M", "DTHSBP", 250, 10.0, 1)
    dt_k0 = _find_row(summary, "V2_FadingResilience_54M", "DTHSBP", 250, 0.0, 1)

    assert float(dt_k0["mac_rx_error_drops"]) > float(dt_k10["mac_rx_error_drops"])
    assert float(dt_k0["c2_decryption_success_rate"]) <= float(dt_k10["c2_decryption_success_rate"]) + 0.02
