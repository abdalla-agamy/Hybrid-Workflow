"""Parse OMNeT++ .sca result files and per-event CSV logs into SimPy-compatible CSV/JSON schemas."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from collections import defaultdict
from pathlib import Path
from typing import Any

SIMPY_SWEEP_COLUMNS = [
    "protocol",
    "swarm_size",
    "p_loss",
    "data_rate_mbps",
    "comm_range_m",
    "num_events",
    "rekey_events",
    "dt_predicted_events",
    "collateral_batches",
    "collateral_leaves_batched",
    "dt_prediction_hit_rate",
    "total_crypto_bytes_matlab",
    "total_bytes_on_air",
    "fragments_sent",
    "retransmissions",
    "mac_collisions",
    "mean_crypto_latency_ms",
    "mean_network_latency_ms",
    "mean_rekey_completion_latency_ms",
    "p95_rekey_completion_latency_ms",
    "mean_pdr",
    "mean_key_sync_ratio",
]

CI_SUMMARY_COLUMNS = [
    "repetitions",
    "mean_rekey_completion_latency_ms_std",
    "mean_rekey_completion_latency_ms_ci95",
    "mean_key_sync_ratio_std",
    "mean_key_sync_ratio_ci95",
    "total_bytes_on_air_std",
    "total_bytes_on_air_ci95",
    "mac_collisions_std",
    "mac_collisions_ci95",
]

T_CRIT_95_TABLE = {
    1: 12.706205,
    2: 4.302653,
    3: 3.182446,
    4: 2.776445,
    5: 2.570582,
    6: 2.446912,
    7: 2.364624,
    8: 2.306004,
    9: 2.262157,
    10: 2.228139,
    14: 2.144787,
    19: 2.093024,
    29: 2.045230,
}

PROTO_TRACE_RE = re.compile(r"events_trace_([A-Za-z0-9]+)_N(\d+)\.json")


def _t_crit_95(df: int) -> float:
    """Return two-sided 95% Student-t critical value t_{0.975, df}."""
    if df <= 0:
        return 0.0
    if df in T_CRIT_95_TABLE:
        return T_CRIT_95_TABLE[df]
    try:
        from scipy.stats import t as student_t  # type: ignore

        return float(student_t.ppf(0.975, df))
    except Exception:
        return 1.959964


def _mean_std_ci95(values: list[float]) -> tuple[float, float, float]:
    """Compute sample mean, sample std (ddof=1), and 95% Student-t CI half-width."""
    r = len(values)
    if r == 0:
        return 0.0, 0.0, 0.0
    mean_v = sum(values) / float(r)
    if r == 1:
        return mean_v, 0.0, 0.0
    var_v = sum((x - mean_v) ** 2 for x in values) / float(r - 1)
    std_v = math.sqrt(max(0.0, var_v))
    ci_v = _t_crit_95(r - 1) * std_v / math.sqrt(float(r))
    return mean_v, std_v, ci_v


def parse_single_sca(sca_path: Path) -> dict[str, Any] | None:
    """Extract one summary row (matching SimPy schema) plus repetition from a single OMNeT++ .sca file."""
    configname = ""
    repetition = 0
    proto = ""
    tp_scalars: dict[str, float] = {}
    mac_collision_sum = 0

    with sca_path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("attr configname "):
                configname = line.split(None, 2)[2].strip().strip('"')
            elif line.startswith("attr repetition "):
                try:
                    repetition = int(line.split(None, 2)[2].strip().strip('"'))
                except ValueError:
                    pass
            elif line.startswith("config *.tracePlayer.traceFile ") or line.startswith("par ") and " traceFile " in line:
                m = PROTO_TRACE_RE.search(line)
                if m:
                    proto = m.group(1)
            elif line.startswith("scalar "):
                parts = line.split()
                if len(parts) < 4:
                    continue
                mod, name, val_str = parts[1], parts[2], parts[3]
                if mod.endswith(".tracePlayer"):
                    try:
                        tp_scalars[name] = float(val_str)
                    except ValueError:
                        pass
                elif ".wlan" in mod and ".mac" in mod:
                    if name in (
                        "packetSentToPeerWithRetry:count",
                        "packetDropIncorrectlyReceived:count",
                        "packetDropRetryLimitReached:count",
                    ):
                        try:
                            mac_collision_sum += int(round(float(val_str)))
                        except ValueError:
                            pass

    if not tp_scalars or not proto or "swarmSize" not in tp_scalars:
        return None

    return {
        "configname": configname,
        "repetition": repetition,
        "protocol": proto,
        "swarm_size": int(round(tp_scalars["swarmSize"])),
        "p_loss": round(float(tp_scalars.get("pLossExtra", 0.0)), 4),
        "data_rate_mbps": float(tp_scalars.get("dataRateMbps", 6.0)),
        "comm_range_m": float(tp_scalars.get("commRangeM", 350.0)),
        "num_events": int(round(tp_scalars.get("numEvents", 0))),
        "rekey_events": int(round(tp_scalars.get("rekeyEvents", 0))),
        "dt_predicted_events": int(round(tp_scalars.get("dtPredictedEvents", 0))),
        "collateral_batches": int(round(tp_scalars.get("collateralBatches", 0))),
        "collateral_leaves_batched": int(round(tp_scalars.get("collateralLeavesBatched", 0))),
        "dt_prediction_hit_rate": float(tp_scalars.get("dtPredictionHitRate", 0.0)),
        "total_crypto_bytes_matlab": int(round(tp_scalars.get("totalCryptoBytesMatlab", 0))),
        "total_bytes_on_air": int(round(tp_scalars.get("totalBytesOnAir", 0))),
        "fragments_sent": int(round(tp_scalars.get("fragmentsSent", 0))),
        "retransmissions": int(round(tp_scalars.get("retransmissions", 0))),
        "mac_collisions": int(mac_collision_sum),
        "mean_crypto_latency_ms": float(tp_scalars.get("meanCryptoLatencyMs", 0.0)),
        "mean_network_latency_ms": float(tp_scalars.get("meanNetworkLatencyMs", 0.0)),
        "mean_rekey_completion_latency_ms": float(tp_scalars.get("meanRekeyCompletionLatencyMs", 0.0)),
        "p95_rekey_completion_latency_ms": float(tp_scalars.get("p95RekeyCompletionLatencyMs", 0.0)),
        "mean_pdr": float(tp_scalars.get("meanPdr", 1.0)),
        "mean_key_sync_ratio": float(tp_scalars.get("meanKeySyncRatio", 1.0)),
    }


def collect_results(results_dir: Path, include_smoke: bool = False) -> dict[str, Any]:
    """Parse all .sca runs and per-event CSV files into unified multi-seed CI CSV + JSON artifacts."""
    sca_files = sorted(results_dir.glob("*.sca"))
    raw_rows: list[dict[str, Any]] = []
    for p in sca_files:
        row = parse_single_sca(p)
        if row is None:
            continue
        if not include_smoke and row["configname"] == "DTHSBP_Smoke":
            continue
        raw_rows.append(row)

    # Prefer replicated parameter-study configs (Scalability, LossResilience) for CI aggregation;
    # keep DynamicStochastic separate for per-event logs unless it is the only data available.
    sweep_rows = [r for r in raw_rows if r["configname"] in ("Scalability", "LossResilience")]
    source_rows = sweep_rows if sweep_rows else raw_rows

    # Deduplicate overlapping (protocol, swarm_size, p_loss, data_rate_mbps, repetition)
    # across Scalability vs LossResilience, then group across repetitions.
    prio = {"Scalability": 0, "LossResilience": 1, "DynamicStochastic": 2, "DTHSBP_Smoke": 3}
    source_rows.sort(key=lambda r: (prio.get(r["configname"], 99), r["repetition"]))
    dedup_rep: dict[tuple[str, int, float, float, int], dict[str, Any]] = {}
    for row in source_rows:
        rep_key = (
            row["protocol"],
            row["swarm_size"],
            round(row["p_loss"], 4),
            round(row["data_rate_mbps"], 2),
            int(row["repetition"]),
        )
        if rep_key not in dedup_rep:
            dedup_rep[rep_key] = row

    grouped: dict[tuple[str, int, float, float], list[dict[str, Any]]] = defaultdict(list)
    for row in dedup_rep.values():
        grp_key = (
            row["protocol"],
            row["swarm_size"],
            round(row["p_loss"], 4),
            round(row["data_rate_mbps"], 2),
        )
        grouped[grp_key].append(row)

    all_columns = SIMPY_SWEEP_COLUMNS + CI_SUMMARY_COLUMNS
    proto_order = {"FlatSBP": 0, "HSBP": 1, "DTHSBP": 2, "LKH": 3, "TGDH": 4}
    agg_rows: list[dict[str, Any]] = []
    rep_counts: list[int] = []

    for grp_key in sorted(
        grouped.keys(),
        key=lambda k: (k[3], k[1], k[2], proto_order.get(k[0], 99)),
    ):
        reps = sorted(grouped[grp_key], key=lambda r: r["repetition"])
        r0 = reps[0]
        r_cnt = len(reps)
        rep_counts.append(r_cnt)

        bytes_mean, bytes_std, bytes_ci = _mean_std_ci95([float(x["total_bytes_on_air"]) for x in reps])
        frags_mean, _, _ = _mean_std_ci95([float(x["fragments_sent"]) for x in reps])
        retx_mean, _, _ = _mean_std_ci95([float(x["retransmissions"]) for x in reps])
        coll_mean, coll_std, coll_ci = _mean_std_ci95([float(x["mac_collisions"]) for x in reps])
        crypto_ms_mean, _, _ = _mean_std_ci95([float(x["mean_crypto_latency_ms"]) for x in reps])
        net_ms_mean, _, _ = _mean_std_ci95([float(x["mean_network_latency_ms"]) for x in reps])
        tot_ms_mean, tot_ms_std, tot_ms_ci = _mean_std_ci95(
            [float(x["mean_rekey_completion_latency_ms"]) for x in reps]
        )
        p95_ms_mean, _, _ = _mean_std_ci95([float(x["p95_rekey_completion_latency_ms"]) for x in reps])
        pdr_mean, _, _ = _mean_std_ci95([float(x["mean_pdr"]) for x in reps])
        sync_mean, sync_std, sync_ci = _mean_std_ci95([float(x["mean_key_sync_ratio"]) for x in reps])

        agg_rows.append(
            {
                "protocol": r0["protocol"],
                "swarm_size": r0["swarm_size"],
                "p_loss": r0["p_loss"],
                "data_rate_mbps": r0["data_rate_mbps"],
                "comm_range_m": r0["comm_range_m"],
                "num_events": r0["num_events"],
                "rekey_events": r0["rekey_events"],
                "dt_predicted_events": r0["dt_predicted_events"],
                "collateral_batches": r0["collateral_batches"],
                "collateral_leaves_batched": r0["collateral_leaves_batched"],
                "dt_prediction_hit_rate": r0["dt_prediction_hit_rate"],
                "total_crypto_bytes_matlab": r0["total_crypto_bytes_matlab"],
                "total_bytes_on_air": round(bytes_mean, 4),
                "fragments_sent": round(frags_mean, 4),
                "retransmissions": round(retx_mean, 4),
                "mac_collisions": round(coll_mean, 4),
                "mean_crypto_latency_ms": round(crypto_ms_mean, 6),
                "mean_network_latency_ms": round(net_ms_mean, 6),
                "mean_rekey_completion_latency_ms": round(tot_ms_mean, 6),
                "p95_rekey_completion_latency_ms": round(p95_ms_mean, 6),
                "mean_pdr": round(pdr_mean, 6),
                "mean_key_sync_ratio": round(sync_mean, 6),
                "repetitions": r_cnt,
                "mean_rekey_completion_latency_ms_std": round(tot_ms_std, 6),
                "mean_rekey_completion_latency_ms_ci95": round(tot_ms_ci, 6),
                "mean_key_sync_ratio_std": round(sync_std, 6),
                "mean_key_sync_ratio_ci95": round(sync_ci, 6),
                "total_bytes_on_air_std": round(bytes_std, 4),
                "total_bytes_on_air_ci95": round(bytes_ci, 4),
                "mac_collisions_std": round(coll_std, 4),
                "mac_collisions_ci95": round(coll_ci, 4),
            }
        )

    sweep_csv = results_dir / "omnet_sweep_results.csv"
    with sweep_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=all_columns)
        writer.writeheader()
        writer.writerows(agg_rows)

    # Combine DynamicStochastic per-event logs into omnet_per_event_metrics.csv
    ev_files = sorted(results_dir.glob("omnet_events_*.csv"))
    combined_events: list[dict[str, str]] = []
    ev_fields = [
        "protocol", "swarm_size", "p_loss", "data_rate_mbps", "t", "event_type",
        "cluster_id", "rekey_count", "is_collateral_batch", "affected_count",
        "prediction_hit", "crypto_bytes", "bytes_on_air", "fragments_sent",
        "retransmissions", "crypto_latency_ms", "network_latency_ms",
        "rekey_completion_latency_ms", "pdr", "key_sync_ratio",
    ]
    for ef in ev_files:
        with ef.open(encoding="utf-8") as f:
            for r in csv.DictReader(f):
                combined_events.append(r)

    per_event_csv = results_dir / "omnet_per_event_metrics.csv"
    with per_event_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=ev_fields)
        writer.writeheader()
        writer.writerows(combined_events)

    max_reps = max(rep_counts) if rep_counts else 0
    summary_payload = {
        "meta": {
            "simulator": "OMNeT++ 6.5.0 + INET 4.7.0 (IEEE 802.11g DCF)",
            "seed": 42,
            "repetitions": max_reps,
            "t_crit_95": round(_t_crit_95(max_reps - 1), 6) if max_reps > 1 else 0.0,
            "comm_range_m": 350.0,
            "carrier_sense_range_m": 700.0,
            "mtu_bytes": 1500,
            "max_retries": 3,
            "sca_files_parsed": len(raw_rows),
            "sweep_sca_files_aggregated": len(dedup_rep),
            "total_runs": len(agg_rows),
            "per_event_rows": len(combined_events),
        },
        "runs": agg_rows,
    }
    summary_json = results_dir / "omnet_summary_metrics.json"
    summary_json.write_text(json.dumps(summary_payload, indent=2), encoding="utf-8")
    return summary_payload


def main() -> None:
    parser = argparse.ArgumentParser(description="Extract OMNeT++/INET scalars into SimPy-compatible CSV/JSON.")
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "results",
        help="Directory containing .sca outputs",
    )
    parser.add_argument("--include-smoke", action="store_true", help="Include DTHSBP_Smoke if no sweep runs present")
    args = parser.parse_args()

    payload = collect_results(args.results_dir.resolve(), include_smoke=args.include_smoke)
    print(
        f"[ParseOmnetScalars] sca_parsed={payload['meta']['sca_files_parsed']} "
        f"unique_runs={payload['meta']['total_runs']} "
        f"per_event_rows={payload['meta']['per_event_rows']}"
    )


if __name__ == "__main__":
    main()
