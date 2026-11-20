"""Parse OMNeT++ .sca result files and per-event CSV logs into SimPy-compatible CSV/JSON schemas."""

from __future__ import annotations

import argparse
import csv
import json
import re
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

PROTO_TRACE_RE = re.compile(r"events_trace_([A-Za-z0-9]+)_N(\d+)\.json")


def parse_single_sca(sca_path: Path) -> dict[str, Any] | None:
    """Extract one summary row (matching SimPy schema) from a single OMNeT++ .sca file."""
    configname = ""
    proto = ""
    tp_scalars: dict[str, float] = {}
    mac_collision_sum = 0

    with sca_path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("attr configname "):
                configname = line.split(None, 2)[2].strip().strip('"')
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
    """Parse all .sca runs and per-event CSV files into unified CSV + JSON artifacts."""
    sca_files = sorted(results_dir.glob("*.sca"))
    raw_rows: list[dict[str, Any]] = []
    for p in sca_files:
        row = parse_single_sca(p)
        if row is None:
            continue
        if not include_smoke and row["configname"] == "DTHSBP_Smoke":
            continue
        raw_rows.append(row)

    # Deduplicate overlapping (protocol, swarm_size, p_loss, data_rate_mbps) points across configs
    # (prefer Scalability > LossResilience > DynamicStochastic > DTHSBP_Smoke)
    prio = {"Scalability": 0, "LossResilience": 1, "DynamicStochastic": 2, "DTHSBP_Smoke": 3}
    raw_rows.sort(key=lambda r: prio.get(r["configname"], 99))
    dedup: dict[tuple[str, int, float, float], dict[str, Any]] = {}
    for row in raw_rows:
        key = (row["protocol"], row["swarm_size"], round(row["p_loss"], 4), round(row["data_rate_mbps"], 2))
        if key not in dedup:
            dedup[key] = row

    proto_order = {"FlatSBP": 0, "HSBP": 1, "DTHSBP": 2, "LKH": 3, "TGDH": 4}
    clean_rows = [
        {k: row[k] for k in SIMPY_SWEEP_COLUMNS}
        for row in sorted(
            dedup.values(),
            key=lambda r: (r["data_rate_mbps"], r["swarm_size"], r["p_loss"], proto_order.get(r["protocol"], 99)),
        )
    ]

    sweep_csv = results_dir / "omnet_sweep_results.csv"
    with sweep_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=SIMPY_SWEEP_COLUMNS)
        writer.writeheader()
        writer.writerows(clean_rows)

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

    summary_payload = {
        "meta": {
            "simulator": "OMNeT++ 6.5.0 + INET 4.7.0 (IEEE 802.11g DCF)",
            "seed": 42,
            "comm_range_m": 350.0,
            "carrier_sense_range_m": 700.0,
            "mtu_bytes": 1500,
            "max_retries": 3,
            "sca_files_parsed": len(raw_rows),
            "total_runs": len(clean_rows),
            "per_event_rows": len(combined_events),
        },
        "runs": clean_rows,
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
