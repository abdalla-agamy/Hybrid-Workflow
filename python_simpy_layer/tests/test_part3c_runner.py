"""Unit tests for Phase 3C: YAML-driven experiment runner and sweep outputs."""

from __future__ import annotations

import csv
import json
from pathlib import Path

from src.experiment_runner import run_sweep_from_config

LAYER_ROOT = Path(__file__).resolve().parents[1]


def test_small_yaml_sweep_outputs_and_schema(tmp_path: Path) -> None:
    mini_cfg = {
        "seed": 42,
        "comm_range_m": 350.0,
        "carrier_sense_range_m": 700.0,
        "mtu_bytes": 1500,
        "max_retries": 3,
        "enable_multihop_relay": True,
        "p_loss": [0.0, 0.10],
        "data_rate_mbps": [6.0],
        "protocols": ["HSBP", "DTHSBP"],
        "swarm_sizes": [50],
        "traces_dir": str((LAYER_ROOT.parent / "traces" / "samples").resolve()),
        "fallback_samples_dir": str((LAYER_ROOT.parent / "traces" / "samples").resolve()),
        "results_dir": str(tmp_path),
        "validate_traces": False,
    }

    payload = run_sweep_from_config(mini_cfg, LAYER_ROOT / "configs")
    assert payload["meta"]["total_runs"] == 4
    assert len(payload["runs"]) == 4

    sweep_csv = tmp_path / "simpy_sweep_results.csv"
    summary_json = tmp_path / "simpy_summary_metrics.json"
    per_event_csv = tmp_path / "simpy_per_event_metrics.csv"

    assert sweep_csv.exists() and sweep_csv.stat().st_size > 0
    assert summary_json.exists() and summary_json.stat().st_size > 0
    assert per_event_csv.exists() and per_event_csv.stat().st_size > 0

    with sweep_csv.open(encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    assert len(rows) == 4
    required_cols = {
        "protocol",
        "swarm_size",
        "p_loss",
        "data_rate_mbps",
        "total_bytes_on_air",
        "mean_rekey_completion_latency_ms",
        "mean_pdr",
        "mean_key_sync_ratio",
        "mac_collisions",
    }
    assert required_cols.issubset(rows[0].keys())

    parsed_json = json.loads(summary_json.read_text(encoding="utf-8"))
    assert parsed_json["meta"]["total_runs"] == 4
