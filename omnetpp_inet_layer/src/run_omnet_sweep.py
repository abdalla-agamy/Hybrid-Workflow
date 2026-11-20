"""Parallel headless OMNeT++ 6.5.0 Cmdenv sweep runner across Parameter Study configs."""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import subprocess
import time
from pathlib import Path

LAYER_ROOT = Path(__file__).resolve().parents[1]
SIM_DIR = LAYER_ROOT / "simulations"
EXE_PATH = LAYER_ROOT / "dt_hsbp_omnet.exe"
DEFAULT_OMNET_ROOT = Path(os.environ.get("OMNETPP_ROOT", r"F:\omnetpp-6.5.0"))
DEFAULT_INET_ROOT = Path(os.environ.get("INET_ROOT", str(DEFAULT_OMNET_ROOT / "samples" / "inet-4.7.0")))

RUN_LINE_RE = re.compile(r"^Run\s+(\d+):\s+(.*)$")


def _build_omnet_env() -> dict[str, str]:
    env = os.environ.copy()
    extra = [
        str(DEFAULT_OMNET_ROOT / "bin"),
        str(DEFAULT_OMNET_ROOT / "tools" / "win32.x86_64" / "clang64" / "bin"),
        str(DEFAULT_INET_ROOT / "src"),
    ]
    env["PATH"] = os.pathsep.join(extra) + os.pathsep + env.get("PATH", "")
    return env


def query_runs(config_name: str, env: dict[str, str]) -> list[tuple[int, dict[str, str]]]:
    proc = subprocess.run(
        [str(EXE_PATH), "-u", "Cmdenv", "-c", config_name, "-q", "runs", "omnetpp.ini"],
        cwd=str(SIM_DIR),
        env=env,
        capture_output=True,
        text=True,
        check=True,
    )
    runs: list[tuple[int, dict[str, str]]] = []
    for line in proc.stdout.splitlines():
        m = RUN_LINE_RE.match(line.strip())
        if not m:
            continue
        r_idx = int(m.group(1))
        vars_dict: dict[str, str] = {}
        for part in m.group(2).split(","):
            part = part.strip()
            if "=" in part:
                k, v = part.split("=", 1)
                vars_dict[k.strip().lstrip("$")] = v.strip()
        runs.append((r_idx, vars_dict))
    if not runs:
        raise RuntimeError(f"No runs parsed for {config_name}: {proc.stdout}\n{proc.stderr}")
    return runs


def _cleanup_transient_vectors(config_name: str, run_idx: int) -> None:
    """Remove unused .vec and .vci files after a completed run to bound disk usage."""
    results_dir = LAYER_ROOT / "results"
    for ext in ("vec", "vci"):
        for path in results_dir.glob(f"{config_name}-*-{run_idx}.{ext}"):
            path.unlink(missing_ok=True)


def _run_one(task: tuple[str, int, str, dict[str, str]]) -> tuple[str, int, str, int, float, str]:
    config_name, run_idx, label, env = task
    t0 = time.perf_counter()
    proc = subprocess.run(
        [str(EXE_PATH), "-u", "Cmdenv", "-c", config_name, "-r", str(run_idx), "omnetpp.ini"],
        cwd=str(SIM_DIR),
        env=env,
        capture_output=True,
        text=True,
    )
    dt = time.perf_counter() - t0
    msg = ""
    if proc.returncode != 0:
        msg = (proc.stdout + "\n" + proc.stderr)[-800:]
    else:
        _cleanup_transient_vectors(config_name, run_idx)
    return config_name, run_idx, label, proc.returncode, dt, msg


def main() -> None:
    parser = argparse.ArgumentParser(description="Execute OMNeT++ headless parameter studies in parallel.")
    parser.add_argument(
        "--configs",
        nargs="+",
        default=["DynamicStochastic", "Scalability", "LossResilience"],
        help="OMNeT++ ini configurations to run",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=3,
        help="Parallel Cmdenv worker processes (default 3 to keep RAM < 1.5 GB at N=250)",
    )
    parser.add_argument(
        "--no-dedup",
        action="store_true",
        help="Run overlapping points across configs without deduplication",
    )
    args = parser.parse_args()

    if not EXE_PATH.exists():
        raise FileNotFoundError(f"Missing built simulation binary: {EXE_PATH}")

    env = _build_omnet_env()
    seen_grid_points: set[tuple[str, str, str, str]] = set()
    scheduled_tasks: list[tuple[tuple[int, int, float, int], tuple[str, int, str, dict[str, str]]]] = []

    for cfg in args.configs:
        runs = query_runs(cfg, env)
        queued_here = 0
        for r_idx, v in runs:
            n_val = v.get("n", "250")
            rate_val = v.get("rate", "6")
            proto_val = v.get("proto", "")
            ploss_val = v.get("ploss", "0.05")
            key = (proto_val, n_val, rate_val, ploss_val)

            if not args.no_dedup and key in seen_grid_points:
                continue
            seen_grid_points.add(key)
            label = f"{proto_val}_N{n_val}_{rate_val}M_p{ploss_val}"
            # Longest-Processing-Time-First (LPT): start heavy FlatSBP large-N runs early
            lpt_key = (
                0 if proto_val == "FlatSBP" else 1,
                -int(n_val),
                float(rate_val),
                0 if cfg == "DynamicStochastic" else 1,
            )
            scheduled_tasks.append((lpt_key, (cfg, r_idx, label, env)))
            queued_here += 1
        print(f"[OmnetSweep] Config '{cfg}': queued {queued_here}/{len(runs)} runs")

    scheduled_tasks.sort(key=lambda item: item[0])
    tasks = [item[1] for item in scheduled_tasks]

    t_start = time.perf_counter()
    failures: list[tuple[str, int, int, str]] = []
    completed = 0
    total = len(tasks)

    with concurrent.futures.ProcessPoolExecutor(max_workers=max(1, args.workers)) as pool:
        for cfg, run_idx, label, rc, dt, err in pool.map(_run_one, tasks):
            completed += 1
            if rc != 0:
                failures.append((cfg, run_idx, rc, err))
                print(f"  [FAIL] {cfg}#{run_idx} ({label}) rc={rc} ({dt:.1f}s):\n{err}", flush=True)
            else:
                print(f"  [OK {completed}/{total}] {cfg}#{run_idx} ({label}) in {dt:.1f}s", flush=True)

    elapsed = time.perf_counter() - t_start
    print(f"[OmnetSweep] Finished {total} runs in {elapsed:.2f}s with {len(failures)} failures")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
