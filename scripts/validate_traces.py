"""Validate all baseline metrics and exported MATLAB traces against Draft-07 JSON Schemas."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

import jsonschema


def check_bounded_scoping(trace_data: dict[str, Any], filepath: Path) -> None:
    """Enforce bounded nodePositions and coordinate bounds on every event."""
    meta = trace_data["meta"]
    width = float(meta["arenaWidthM"])
    height = float(meta["arenaHeightM"])

    for idx, node in enumerate(trace_data["initialNodes"]):
        if not (0.0 <= node["x"] <= width and 0.0 <= node["y"] <= height):
            raise ValueError(
                f"{filepath.name}: initialNodes[{idx}] out of bounds ({node['x']}, {node['y']})"
            )

    for ev_idx, ev in enumerate(trace_data["events"]):
        n_pos = len(ev["nodePositions"])
        n_mem = len(ev["clusterMembershipAfter"])
        if n_pos > n_mem + 1:
            raise ValueError(
                f"{filepath.name}: event[{ev_idx}] nodePositions ({n_pos}) > clusterMembershipAfter+1 ({n_mem + 1})"
            )

        target_cid = ev["clusterId"]
        for np in ev["nodePositions"]:
            if np["clusterId"] != target_cid and np["role"] != "SL":
                raise ValueError(
                    f"{filepath.name}: event[{ev_idx}] foreign node uavId={np['uavId']} "
                    f"(clusterId={np['clusterId']} != {target_cid}, role={np['role']})"
                )
            if not (0.0 <= np["x"] <= width and 0.0 <= np["y"] <= height):
                raise ValueError(
                    f"{filepath.name}: event[{ev_idx}] uavId={np['uavId']} out of bounds ({np['x']}, {np['y']})"
                )


def main() -> int:
    hybrid_root = Path(__file__).resolve().parents[1]
    schemas_dir = hybrid_root / "traces" / "schemas"

    trace_schema = json.loads((schemas_dir / "swarm_trace_schema.json").read_text(encoding="utf-8"))
    metrics_schema = json.loads((schemas_dir / "matlab_metrics_schema.json").read_text(encoding="utf-8"))

    jsonschema.Draft7Validator.check_schema(trace_schema)
    jsonschema.Draft7Validator.check_schema(metrics_schema)

    baseline_path = hybrid_root / "baseline_matlab_results" / "matlab_baseline_metrics.json"
    baseline_data = json.loads(baseline_path.read_text(encoding="utf-8"))
    jsonschema.validate(instance=baseline_data, schema=metrics_schema)
    print(f"[PASS] Baseline metrics valid: {baseline_path.relative_to(hybrid_root)}")

    trace_files = sorted(
        list((hybrid_root / "traces" / "samples").glob("*.json"))
        + list((hybrid_root / "traces" / "runs").glob("*.json"))
    )
    if not trace_files:
        print("[FAIL] No exported trace files found in traces/samples or traces/runs!", file=sys.stderr)
        return 1

    validator = jsonschema.Draft7Validator(trace_schema)
    for tf in trace_files:
        data = json.loads(tf.read_text(encoding="utf-8"))
        validator.validate(data)
        check_bounded_scoping(data, tf)
        print(
            f"[PASS] {tf.relative_to(hybrid_root)} "
            f"(proto={data['meta']['protocol']}, N={data['meta']['swarmSize']}, events={len(data['events'])})"
        )

    print(f"\nValidated {len(trace_files)} trace files and baseline metrics: 100% PASS.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
