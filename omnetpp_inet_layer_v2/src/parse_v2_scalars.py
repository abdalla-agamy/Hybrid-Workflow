"""Parse OMNeT++ v2 closed-loop .sca files and aggregate 95% Student-t CI summaries."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from collections import defaultdict
from pathlib import Path
from typing import Any

PROTO_FROM_CODE = {
    0: "FlatSBP",
    1: "HSBP",
    2: "DTHSBP",
    3: "LKH",
    4: "TGDH",
}

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
}

METRIC_COLUMNS = [
    "rekey_rounds",
    "dt_proactive_rounds",
    "collateral_batches",
    "collateral_leaves_batched",
    "reactive_rounds",
    "sl_failover_completed",
    "dt_prediction_hit_rate",
    "rekey_bytes_on_air",
    "control_dt_bytes_on_air",
    "c2_bytes_on_air",
    "total_bytes_on_air",
    "rekey_fragments_sent",
    "rekey_acks_sent",
    "key_resync_pulls_sent",
    "relays_suppressed",
    "mean_crypto_latency_ms",
    "mean_rekey_completion_latency_ms",
    "p95_rekey_completion_latency_ms",
    "mean_c2_latency_ms",
    "c2_bcasts_sent",
    "c2_expected_receptions",
    "c2_decrypted_ok",
    "c2_decryption_success_rate",
    "mean_exposure_window_packets",
    "total_exposure_window_packets",
    "mean_desync_outage_duration_ms",
    "p95_desync_outage_duration_ms",
    "rekey_pdr",
    "edca_ac_be_tx",
    "edca_ac_vi_tx",
    "edca_ac_vo_tx",
    "dcf_tx",
    "mac_queue_overflow_drops",
    "mac_retry_drops",
    "mac_rx_error_drops",
]

SCALAR_CAMEL_TO_SNAKE = {
    "rekeyRounds": "rekey_rounds",
    "dtProactiveRounds": "dt_proactive_rounds",
    "collateralBatches": "collateral_batches",
    "collateralLeavesBatched": "collateral_leaves_batched",
    "reactiveRounds": "reactive_rounds",
    "slFailoverCompleted": "sl_failover_completed",
    "dtPredictionHitRate": "dt_prediction_hit_rate",
    "rekeyBytesOnAir": "rekey_bytes_on_air",
    "controlDtBytesOnAir": "control_dt_bytes_on_air",
    "c2BytesOnAir": "c2_bytes_on_air",
    "totalBytesOnAir": "total_bytes_on_air",
    "rekeyFragmentsSent": "rekey_fragments_sent",
    "rekeyAcksSent": "rekey_acks_sent",
    "keyResyncPullsSent": "key_resync_pulls_sent",
    "relaysSuppressed": "relays_suppressed",
    "meanCryptoLatencyMs": "mean_crypto_latency_ms",
    "meanRekeyCompletionLatencyMs": "mean_rekey_completion_latency_ms",
    "p95RekeyCompletionLatencyMs": "p95_rekey_completion_latency_ms",
    "meanC2LatencyMs": "mean_c2_latency_ms",
    "c2BcastsSent": "c2_bcasts_sent",
    "c2ExpectedReceptions": "c2_expected_receptions",
    "c2DecryptedOk": "c2_decrypted_ok",
    "c2DecryptionSuccessRate": "c2_decryption_success_rate",
    "meanExposureWindowPackets": "mean_exposure_window_packets",
    "totalExposureWindowPackets": "total_exposure_window_packets",
    "meanDesyncOutageDurationMs": "mean_desync_outage_duration_ms",
    "p95DesyncOutageDurationMs": "p95_desync_outage_duration_ms",
    "rekeyPdr": "rekey_pdr",
}

EDCAF_AC_RE = re.compile(r"\.edcaf\[(\d+)\](?:\.|$)")


def _t_crit_95(df: int) -> float:
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
    configname = ""
    repetition = 0
    runnumber = 0
    sm_scalars: dict[str, float] = {}
    edca_ac_tx = {0: 0.0, 1: 0.0, 2: 0.0, 3: 0.0}
    dcf_tx = 0.0
    q_overflow = 0.0
    retry_drops = 0.0
    rx_err_drops = 0.0

    for line in sca_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("attr configname "):
            configname = line.split(" ", 2)[2].strip()
        elif line.startswith("attr repetition "):
            repetition = int(line.split(" ", 2)[2].strip())
        elif line.startswith("attr runnumber "):
            runnumber = int(line.split(" ", 2)[2].strip())
        elif line.startswith("scalar "):
            parts = line.split()
            if len(parts) < 4:
                continue
            mod_name, field_name = parts[1], parts[2]
            try:
                val = float(parts[3])
            except ValueError:
                continue
            if math.isnan(val):
                val = 0.0

            if mod_name.endswith(".scenarioManager"):
                sm_scalars[field_name] = val
            elif ".mac.hcf.edca.edcaf[" in mod_name and field_name == "packetSentToPeer:count":
                m = EDCAF_AC_RE.search(mod_name)
                if m:
                    ac = int(m.group(1))
                    if ac in edca_ac_tx:
                        edca_ac_tx[ac] += val
            elif ".mac.dcf" in mod_name and field_name == "packetSentToPeer:count":
                dcf_tx += val
            elif field_name == "packetDropQueueOverflow:count":
                q_overflow += val
            elif field_name == "packetDropRetryLimitReached:count":
                retry_drops += val
            elif field_name == "packetDropIncorrectlyReceived:count":
                rx_err_drops += val

    if not configname or "c2DecryptionSuccessRate" not in sm_scalars:
        return None

    proto_code = int(round(sm_scalars.get("protocolCode", 2)))
    row: dict[str, Any] = {
        "config": configname,
        "run_number": runnumber,
        "repetition": repetition,
        "protocol": PROTO_FROM_CODE.get(proto_code, "DTHSBP"),
        "swarm_size": int(round(sm_scalars.get("initialSwarmSize", 50))),
        "rician_k_db": round(sm_scalars.get("ricianKDb", 4.0), 2),
        "qos_enabled": int(round(sm_scalars.get("qosEnabled", 1.0))),
    }
    for camel, snake in SCALAR_CAMEL_TO_SNAKE.items():
        row[snake] = sm_scalars.get(camel, 0.0)

    row["edca_ac_be_tx"] = edca_ac_tx[1]
    row["edca_ac_vi_tx"] = edca_ac_tx[2]
    row["edca_ac_vo_tx"] = edca_ac_tx[3]
    row["dcf_tx"] = dcf_tx
    row["mac_queue_overflow_drops"] = q_overflow
    row["mac_retry_drops"] = retry_drops
    row["mac_rx_error_drops"] = rx_err_drops
    return row


def aggregate_runs(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str, int, float, int], list[dict[str, Any]]] = defaultdict(list)
    for r in rows:
        key = (
            str(r["config"]),
            str(r["protocol"]),
            int(r["swarm_size"]),
            float(r["rician_k_db"]),
            int(r["qos_enabled"]),
        )
        grouped[key].append(r)

    summary: list[dict[str, Any]] = []
    for (cfg, proto, n_val, k_db, qos), grp in sorted(grouped.items()):
        out: dict[str, Any] = {
            "config": cfg,
            "protocol": proto,
            "swarm_size": n_val,
            "rician_k_db": k_db,
            "qos_enabled": qos,
            "repetitions": len(grp),
        }
        for col in METRIC_COLUMNS:
            vals = [float(item[col]) for item in grp]
            mean_v, std_v, ci_v = _mean_std_ci95(vals)
            out[col] = round(mean_v, 6)
            out[f"{col}_std"] = round(std_v, 6)
            out[f"{col}_ci95"] = round(ci_v, 6)
        summary.append(out)

    # Synthesize shared baseline views from V2_Scalability_54M (identical seed-set=0..4, k=4 dB, qos=1)
    by_key = {
        (r["config"], r["protocol"], int(r["swarm_size"]), float(r["rician_k_db"]), int(r["qos_enabled"])): r
        for r in summary
    }
    for proto in ("FlatSBP", "HSBP", "DTHSBP", "LKH", "TGDH"):
        src_key = ("V2_Scalability_54M", proto, 250, 4.0, 1)
        fading_key = ("V2_FadingResilience_54M", proto, 250, 4.0, 1)
        if src_key in by_key and fading_key not in by_key:
            cloned = dict(by_key[src_key])
            cloned["config"] = "V2_FadingResilience_54M"
            summary.append(cloned)
            by_key[fading_key] = cloned

    for proto in ("DTHSBP", "FlatSBP"):
        for n_val in (100, 250):
            src_key = ("V2_Scalability_54M", proto, n_val, 4.0, 1)
            ablation_key = ("V2_EDCA_vs_DCF_Ablation", proto, n_val, 4.0, 1)
            if src_key in by_key and ablation_key not in by_key:
                cloned = dict(by_key[src_key])
                cloned["config"] = "V2_EDCA_vs_DCF_Ablation"
                summary.append(cloned)
                by_key[ablation_key] = cloned

    summary.sort(key=lambda r: (r["config"], r["protocol"], int(r["swarm_size"]), float(r["rician_k_db"]), int(r["qos_enabled"])))
    return summary


def main() -> int:
    layer_root = Path(__file__).resolve().parents[1]
    results_dir = layer_root / "results"

    parser = argparse.ArgumentParser(description="Parse OMNeT++ v2 .sca files into CSV/JSON summaries")
    parser.add_argument("--results-dir", type=Path, default=results_dir)
    parser.add_argument(
        "--configs",
        nargs="+",
        default=["V2_Scalability_54M", "V2_FadingResilience_54M", "V2_EDCA_vs_DCF_Ablation"],
    )
    args = parser.parse_args()

    allowed_cfgs = set(args.configs)
    raw_rows: list[dict[str, Any]] = []
    for sca_path in sorted(args.results_dir.glob("*.sca")):
        parsed = parse_single_sca(sca_path)
        if parsed and parsed["config"] in allowed_cfgs:
            raw_rows.append(parsed)

    if not raw_rows:
        raise RuntimeError(f"No matching v2 .sca files found in {args.results_dir} for {sorted(allowed_cfgs)}")

    raw_rows.sort(key=lambda r: (r["config"], r["protocol"], r["swarm_size"], r["rician_k_db"], r["qos_enabled"], r["repetition"]))
    summary_rows = aggregate_runs(raw_rows)

    raw_csv = args.results_dir / "v2_raw_runs.csv"
    raw_cols = ["config", "run_number", "repetition", "protocol", "swarm_size", "rician_k_db", "qos_enabled"] + METRIC_COLUMNS
    with raw_csv.open("w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=raw_cols)
        w.writeheader()
        w.writerows(raw_rows)

    sum_cols = ["config", "protocol", "swarm_size", "rician_k_db", "qos_enabled", "repetitions"]
    for col in METRIC_COLUMNS:
        sum_cols.extend([col, f"{col}_std", f"{col}_ci95"])

    sum_csv = args.results_dir / "v2_summary_ci95.csv"
    with sum_csv.open("w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=sum_cols)
        w.writeheader()
        w.writerows(summary_rows)

    sum_json = args.results_dir / "v2_summary_ci95.json"
    sum_json.write_text(json.dumps({"summary": summary_rows}, indent=2), encoding="utf-8")

    print(f"Parsed {len(raw_rows)} raw v2 runs -> {len(summary_rows)} aggregated CI95 rows.")
    print(f"Wrote {raw_csv}, {sum_csv}, {sum_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
