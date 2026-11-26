"""Parallel headless OMNeT++ 6.5.0 Cmdenv sweep runner for DT-HSBP v2 closed-loop study."""

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
EXE_PATH = LAYER_ROOT / "dt_hsbp_omnet_v2.exe"
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
    results_dir = LAYER_ROOT / "results"
    for ext in ("vec", "vci"):
        for path in results_dir.glob(f"{config_name}-*-{run_idx}.{ext}"):
            path.unlink(missing_ok=True)


def _is_run_complete(config_name: str, run_idx: int, rep_val: str) -> bool:
    results_dir = LAYER_ROOT / "results"
    sca_path = results_dir / f"{config_name}-{rep_val}-{run_idx}.sca"
    if not sca_path.exists():
        return False
    try:
        return "c2DecryptionSuccessRate" in sca_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


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


def main() -> int:
    parser = argparse.ArgumentParser(description="Run DT-HSBP v2 closed-loop OMNeT++ parameter sweeps")
    parser.add_argument(
        "--configs",
        nargs="+",
        default=["V2_Scalability_54M", "V2_FadingResilience_54M", "V2_EDCA_vs_DCF_Ablation"],
        help="OMNeT++ ini configurations to execute",
    )
    parser.add_argument("--workers", type=int, default=4, help="Parallel worker processes")
    parser.add_argument("--max-runs", type=int, default=0, help="Optional cap on number of runs (0 = all)")
    parser.add_argument("--force", action="store_true", help="Re-run even if valid .sca exists")
    args = parser.parse_args()

    if not EXE_PATH.exists():
        raise FileNotFoundError(f"OMNeT++ binary not found at {EXE_PATH}. Build with build_v2_omnet.ps1 first.")

    (LAYER_ROOT / "results").mkdir(parents=True, exist_ok=True)
    env = _build_omnet_env()

    tasks: list[tuple[str, int, str, dict[str, str]]] = []
    skipped = 0
    for cfg in args.configs:
        for r_idx, vars_dict in query_runs(cfg, env):
            rep_val = vars_dict.get("repetition", "0")
            if not args.force and _is_run_complete(cfg, r_idx, rep_val):
                skipped += 1
                continue
            label = ", ".join(f"{k}={v}" for k, v in sorted(vars_dict.items()))
            tasks.append((cfg, r_idx, label, env))

    if args.max_runs > 0:
        tasks = tasks[: args.max_runs]

    print(f"Scheduled {len(tasks)} OMNeT++ v2 runs (skipped {skipped} completed) with {args.workers} workers.")
    if not tasks:
        return 0

    t_start = time.perf_counter()
    failures: list[tuple[str, int, str, str]] = []
    done_cnt = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(_run_one, t) for t in tasks]
        for fut in concurrent.futures.as_completed(futures):
            cfg, r_idx, label, rc, dt, msg = fut.result()
            done_cnt += 1
            if rc != 0:
                failures.append((cfg, r_idx, label, msg))
                print(f"[{done_cnt}/{len(tasks)}] FAIL {cfg}#{r_idx} ({label}) in {dt:.1f}s")
            else:
                print(f"[{done_cnt}/{len(tasks)}] OK   {cfg}#{r_idx} ({label}) in {dt:.1f}s")

    total_dt = time.perf_counter() - t_start
    print(f"Completed {len(tasks)} runs in {total_dt:.1f}s ({len(failures)} failures).")
    for cfg, r_idx, label, msg in failures:
        print(f"--- FAIL {cfg}#{r_idx} ({label}) ---\n{msg}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
