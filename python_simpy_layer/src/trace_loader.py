"""Typed JSON trace loader with Draft-07 schema validation."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import jsonschema

DEFAULT_SCHEMA_PATH = (
    Path(__file__).resolve().parents[2] / "traces" / "schemas" / "swarm_trace_schema.json"
)


@dataclass(frozen=True)
class TraceMeta:
    seed: int
    swarm_size: int
    num_clusters: int
    protocol: str
    simulation_time: float
    time_step: float
    arena_width_m: float
    arena_height_m: float
    max_velocity_mps: float
    key_size: int
    header_size: int
    control_size: int
    message_size: int
    packet_size: int
    mtu: int
    generated_by: str
    generated_at: str


@dataclass(frozen=True)
class InitialNode:
    uav_id: int
    cluster_id: int
    role: str
    x: float
    y: float
    vx: float
    vy: float
    energy: float


@dataclass(frozen=True)
class NodePositionSnapshot:
    uav_id: int
    cluster_id: int
    role: str
    x: float
    y: float


@dataclass(frozen=True)
class HopDescriptor:
    src_role: str
    dst_role: str
    bytes_len: int
    kind: str
    fanout: int
    fragments_count: int


@dataclass(frozen=True)
class CryptoCost:
    leader_time_ms: float
    rekey_count: int
    messages_sent: int
    bytes_transmitted: int


@dataclass(frozen=True)
class TraceEvent:
    t: float
    type: str
    cluster_id: int
    affected_uavs: tuple[int, ...]
    is_collateral_batch: bool
    prediction_hit: bool | None
    node_positions: tuple[NodePositionSnapshot, ...]
    hops: tuple[HopDescriptor, ...]
    crypto: CryptoCost
    cluster_membership_after: tuple[int, ...]


@dataclass(frozen=True)
class SwarmTrace:
    meta: TraceMeta
    initial_nodes: tuple[InitialNode, ...]
    events: tuple[TraceEvent, ...]
    source_path: Path | None = None


def _parse_meta(raw: dict[str, Any]) -> TraceMeta:
    return TraceMeta(
        seed=int(raw["seed"]),
        swarm_size=int(raw["swarmSize"]),
        num_clusters=int(raw["numClusters"]),
        protocol=str(raw["protocol"]),
        simulation_time=float(raw["simulationTime"]),
        time_step=float(raw["timeStep"]),
        arena_width_m=float(raw["arenaWidthM"]),
        arena_height_m=float(raw["arenaHeightM"]),
        max_velocity_mps=float(raw["maxVelocityMps"]),
        key_size=int(raw["keySize"]),
        header_size=int(raw["headerSize"]),
        control_size=int(raw["controlSize"]),
        message_size=int(raw["messageSize"]),
        packet_size=int(raw["packetSize"]),
        mtu=int(raw["mtu"]),
        generated_by=str(raw["generatedBy"]),
        generated_at=str(raw["generatedAt"]),
    )


def _parse_initial_node(raw: dict[str, Any]) -> InitialNode:
    return InitialNode(
        uav_id=int(raw["uavId"]),
        cluster_id=int(raw["clusterId"]),
        role=str(raw["role"]),
        x=float(raw["x"]),
        y=float(raw["y"]),
        vx=float(raw["vx"]),
        vy=float(raw["vy"]),
        energy=float(raw["energy"]),
    )


def _parse_event(raw: dict[str, Any]) -> TraceEvent:
    snapshots = tuple(
        NodePositionSnapshot(
            uav_id=int(np["uavId"]),
            cluster_id=int(np["clusterId"]),
            role=str(np["role"]),
            x=float(np["x"]),
            y=float(np["y"]),
        )
        for np in raw["nodePositions"]
    )
    hops = tuple(
        HopDescriptor(
            src_role=str(h["from"]),
            dst_role=str(h["to"]),
            bytes_len=int(h["bytes"]),
            kind=str(h["kind"]),
            fanout=int(h["fanout"]),
            fragments_count=int(h["fragmentsCount"]),
        )
        for h in raw["hops"]
    )
    c_raw = raw["crypto"]
    crypto = CryptoCost(
        leader_time_ms=float(c_raw["leaderTimeMs"]),
        rekey_count=int(c_raw["rekeyCount"]),
        messages_sent=int(c_raw["messagesSent"]),
        bytes_transmitted=int(c_raw["bytesTransmitted"]),
    )
    return TraceEvent(
        t=float(raw["t"]),
        type=str(raw["type"]),
        cluster_id=int(raw["clusterId"]),
        affected_uavs=tuple(int(u) for u in raw["affectedUAVs"]),
        is_collateral_batch=bool(raw["isCollateralBatch"]),
        prediction_hit=raw["predictionHit"],
        node_positions=snapshots,
        hops=hops,
        crypto=crypto,
        cluster_membership_after=tuple(int(u) for u in raw["clusterMembershipAfter"]),
    )


def load_trace(
    trace_path: str | Path,
    schema_path: str | Path | None = None,
    validate: bool = True,
) -> SwarmTrace:
    """Load and optionally validate a MATLAB-exported JSON swarm trace."""
    t_path = Path(trace_path)
    raw = json.loads(t_path.read_text(encoding="utf-8"))

    if validate:
        s_path = Path(schema_path) if schema_path is not None else DEFAULT_SCHEMA_PATH
        schema = json.loads(s_path.read_text(encoding="utf-8"))
        jsonschema.validate(instance=raw, schema=schema)

    return SwarmTrace(
        meta=_parse_meta(raw["meta"]),
        initial_nodes=tuple(_parse_initial_node(n) for n in raw["initialNodes"]),
        events=tuple(_parse_event(ev) for ev in raw["events"]),
        source_path=t_path,
    )
