"""YAML-driven parallel parameter sweep runner for the Python SimPy wireless simulator."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import os
import time
from dataclasses import asdict
from pathlib import Path
from typing import Any

import yaml

from .protocol_replay_agents import (
    EventMetrics,
    SimulationConfig,
    SimulationSummary,
    replay_trace,
)
from .trace_loader import load_trace


def resolve_trace_path(
    base_dir: Path,
    traces_dir: str,
    fallback_dir: str,
    protocol: str,
    swarm_size: int,
) -> Path:
    primary = (base_dir / traces_dir / f"events_trace_{protocol}_N{swarm_size}.json").resolve()
    if primary.exists():
        return primary
    fallback = (base_dir / fallback_dir / f"events_trace_{protocol}_N{swarm_size}.json").resolve()
    if fallback.exists():
        return fallback
    raise FileNotFoundError(
        f"Trace file not found for protocol={protocol}, N={swarm_size}: checked {primary} and {fallback}"
    )


def _run_trace_family_task(task_args: dict[str, Any]) -> tuple[int, list[dict[str, Any]], list[dict[str, Any]]]:
    """Load one (protocol, swarm_size) trace and replay all (rate, p_loss) combinations."""
    order_idx = int(task_args["order_idx"])
    trace = load_trace(Path(task_args["trace_path"]), validate=bool(task_args["validate"]))

    summaries: list[dict[str, Any]] = []
    events_out: list[dict[str, Any]] = []

    for rate in task_args["data_rates"]:
        for p_loss in task_args["p_losses"]:
            sim_cfg = SimulationConfig(
                p_loss=float(p_loss),
                data_rate_mbps=float(rate),
                comm_range_m=float(task_args["comm_range"]),
                carrier_sense_range_m=float(task_args["cs_range"]),
                mtu_bytes=int(task_args["mtu_bytes"]),
                max_retries=int(task_args["max_retries"]),
                seed=int(task_args["seed"]),
                enable_multihop_relay=bool(task_args["enable_relay"]),
            )
            summary, events = replay_trace(trace, sim_cfg)
            summaries.append(summary.to_dict())
            events_out.extend(asdict(ev) for ev in events)

    return order_idx, summaries, events_out


def run_sweep_from_config(cfg_data: dict[str, Any], config_dir: Path) -> dict[str, Any]:
    """Execute full (protocol x swarm_size x data_rate_mbps x p_loss) SimPy sweep."""
    t_start = time.perf_counter()

    seed = int(cfg_data.get("seed", 42))
    comm_range = float(cfg_data.get("comm_range_m", 350.0))
    cs_range = float(cfg_data.get("carrier_sense_range_m", 700.0))
    mtu_bytes = int(cfg_data.get("mtu_bytes", 1500))
    max_retries = int(cfg_data.get("max_retries", 3))
    enable_relay = bool(cfg_data.get("enable_multihop_relay", True))
    validate = bool(cfg_data.get("validate_traces", False))

    p_losses = [float(x) for x in cfg_data["p_loss"]]
    data_rates = [float(x) for x in cfg_data["data_rate_mbps"]]
    protocols = [str(x) for x in cfg_data["protocols"]]
    swarm_sizes = [int(x) for x in cfg_data["swarm_sizes"]]

    layer_root = config_dir.parent if config_dir.name == "configs" else config_dir
    traces_dir = str(cfg_data.get("traces_dir", "../traces/runs"))
    fallback_dir = str(cfg_data.get("fallback_samples_dir", "../traces/samples"))
    results_dir = (layer_root / str(cfg_data.get("results_dir", "results"))).resolve()
    results_dir.mkdir(parents=True, exist_ok=True)

    tasks: list[dict[str, Any]] = []
    order_idx = 0
    for n in swarm_sizes:
        for proto in protocols:
            t_path = resolve_trace_path(layer_root, traces_dir, fallback_dir, proto, n)
            tasks.append(
                {
                    "order_idx": order_idx,
                    "trace_path": str(t_path),
                    "validate": validate,
                    "data_rates": data_rates,
                    "p_losses": p_losses,
                    "comm_range": comm_range,
                    "cs_range": cs_range,
                    "mtu_bytes": mtu_bytes,
                    "max_retries": max_retries,
                    "seed": seed,
                    "enable_relay": enable_relay,
                }
            )
            order_idx += 1

    results_by_order: dict[int, tuple[list[dict[str, Any]], list[dict[str, Any]]]] = {}
    max_workers = int(cfg_data.get("max_workers", min(8, os.cpu_count() or 4)))

    if len(tasks) > 2 and max_workers > 1:
        with concurrent.futures.ProcessPoolExecutor(max_workers=max_workers) as pool:
            for idx, s_list, e_list in pool.map(_run_trace_family_task, tasks):
                results_by_order[idx] = (s_list, e_list)
    else:
        for t in tasks:
            idx, s_list, e_list = _run_trace_family_task(t)
            results_by_order[idx] = (s_list, e_list)

    summary_dicts: list[dict[str, Any]] = []
    event_dicts: list[dict[str, Any]] = []
    for idx in range(len(tasks)):
        s_list, e_list = results_by_order[idx]
        summary_dicts.extend(s_list)
        event_dicts.extend(e_list)

    elapsed_s = time.perf_counter() - t_start

    sweep_csv = results_dir / "simpy_sweep_results.csv"
    summary_json = results_dir / "simpy_summary_metrics.json"
    per_event_csv = results_dir / "simpy_per_event_metrics.csv"

    if summary_dicts:
        with sweep_csv.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=list(summary_dicts[0].keys()))
            writer.writeheader()
            writer.writerows(summary_dicts)

    if event_dicts:
        with per_event_csv.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=list(event_dicts[0].keys()))
            writer.writeheader()
            writer.writerows(event_dicts)

    payload = {
        "meta": {
            "seed": seed,
            "comm_range_m": comm_range,
            "carrier_sense_range_m": cs_range,
            "mtu_bytes": mtu_bytes,
            "max_retries": max_retries,
            "total_runs": len(summary_dicts),
            "total_events": len(event_dicts),
            "elapsed_seconds": round(elapsed_s, 3),
        },
        "runs": summary_dicts,
    }
    summary_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return payload


def run_from_yaml(config_path: str | Path) -> dict[str, Any]:
    cfg_path = Path(config_path).resolve()
    cfg_data = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    return run_sweep_from_config(cfg_data, cfg_path.parent)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run SimPy wireless replay parameter sweep.")
    parser.add_argument(
        "--config",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "configs" / "simpy_experiments.yaml",
        help="Path to YAML experiment configuration",
    )
    args = parser.parse_args()
    payload = run_from_yaml(args.config)
    print(
        f"[SimPySweep] runs={payload['meta']['total_runs']} "
        f"events={payload['meta']['total_events']} "
        f"elapsed={payload['meta']['elapsed_seconds']:.2f}s"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
