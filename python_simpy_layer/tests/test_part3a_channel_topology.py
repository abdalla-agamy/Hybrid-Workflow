"""Unit tests for Phase 3A: trace loader, carry-forward topology manager, and wireless channel."""

from __future__ import annotations

import copy
from pathlib import Path

import pytest

from src.topology_manager import TopologyManager
from src.trace_loader import (
    CryptoCost,
    NodePositionSnapshot,
    TraceEvent,
    load_trace,
)
from src.wireless_channel import ChannelConfig, SPEED_OF_LIGHT_MPS, WirelessChannel

HYBRID_ROOT = Path(__file__).resolve().parents[2]
SAMPLES_DIR = HYBRID_ROOT / "traces" / "samples"


def test_load_and_validate_all_sample_traces() -> None:
    protocols = ["FlatSBP", "HSBP", "DTHSBP", "LKH", "TGDH"]
    for proto in protocols:
        trace = load_trace(SAMPLES_DIR / f"events_trace_{proto}_N50.json", validate=True)
        assert trace.meta.protocol == proto
        assert trace.meta.swarm_size == 50
        assert len(trace.initial_nodes) == 50
        assert len(trace.events) > 0


def test_bounded_position_carry_forward_across_clusters() -> None:
    trace = load_trace(SAMPLES_DIR / "events_trace_DTHSBP_N50.json", validate=True)
    topo = TopologyManager(trace)

    # Record initial coordinates of cluster 5 members
    cluster5_initial = {
        uid: topo.get_position(uid) for uid in topo.cluster_members[5]
    }
    assert len(cluster5_initial) > 0

    # Find first event strictly targeting cluster 2 (not touching cluster 5)
    c2_event = next(ev for ev in trace.events if ev.cluster_id == 2)
    snapshot_ids = {np.uav_id for np in c2_event.node_positions}
    assert all(uid not in snapshot_ids for uid in cluster5_initial)

    topo.apply_event(c2_event)

    # Uninvolved cluster 5 positions must be carried forward intact
    for uid, pos in cluster5_initial.items():
        assert topo.get_position(uid) == pos

    # Involved nodes in ev.node_positions must match exact snapshot coordinates
    for snap in c2_event.node_positions:
        assert topo.get_position(snap.uav_id) == pytest.approx((snap.x, snap.y))


def test_ch_reassignment_on_head_departure() -> None:
    trace = load_trace(SAMPLES_DIR / "events_trace_HSBP_N50.json", validate=True)
    topo = TopologyManager(trace)

    old_ch = topo.get_ch_id(3)
    assert old_ch is not None
    remaining = [uid for uid in topo.cluster_members[3] if uid != old_ch]
    assert len(remaining) >= 2

    # Craft departure event where CH leaves and snapshots omit CH role tag (edge fallback test)
    snaps = tuple(
        NodePositionSnapshot(uav_id=uid, cluster_id=3, role="follower", x=100.0 + i, y=200.0)
        for i, uid in enumerate(remaining)
    )
    dep_ev = TraceEvent(
        t=5.0,
        type="leave",
        cluster_id=3,
        affected_uavs=(old_ch,),
        is_collateral_batch=False,
        prediction_hit=None,
        node_positions=snaps,
        hops=(),
        crypto=CryptoCost(leader_time_ms=0.1, rekey_count=1, messages_sent=len(remaining), bytes_transmitted=len(remaining) * 56),
        cluster_membership_after=tuple(remaining),
    )
    topo.apply_event(dep_ev)

    assert not topo.nodes[old_ch].active
    new_ch = topo.get_ch_id(3)
    assert new_ch is not None and new_ch != old_ch and new_ch in remaining

    # Emptying the entire cluster sets cluster head to None
    empty_ev = copy.deepcopy(dep_ev)
    empty_ev = TraceEvent(
        t=6.0,
        type="leave",
        cluster_id=3,
        affected_uavs=tuple(remaining),
        is_collateral_batch=False,
        prediction_hit=None,
        node_positions=(),
        hops=(),
        crypto=CryptoCost(leader_time_ms=0.0, rekey_count=0, messages_sent=0, bytes_transmitted=0),
        cluster_membership_after=(),
    )
    topo.apply_event(empty_ev)
    assert topo.get_ch_id(3) is None


def test_connectivity_graph_within_range() -> None:
    trace = load_trace(SAMPLES_DIR / "events_trace_DTHSBP_N50.json", validate=True)
    topo = TopologyManager(trace)
    rng_m = 350.0
    g = topo.build_connectivity_graph(comm_range_m=rng_m)

    assert g.number_of_nodes() == 50
    for u, v, data in g.edges(data=True):
        assert data["distance_m"] <= rng_m + 1e-9
        assert topo.distance(u, v) <= rng_m + 1e-9

    nodes = list(g.nodes)
    for idx, u in enumerate(nodes):
        for v in nodes[idx + 1:]:
            if not g.has_edge(u, v):
                assert topo.distance(u, v) > rng_m


def test_wireless_channel_per_and_propagation_bounds() -> None:
    for p_loss in (0.0, 0.05, 0.10, 0.15):
        ch = WirelessChannel(ChannelConfig(base_p_loss=p_loss, comm_range_m=350.0, seed=7))

        for d in (0.0, 25.0, 50.0):
            assert ch.compute_per(d) <= p_loss + 0.02 + 1e-12

        for d in (351.0, 500.0, 900.0):
            assert ch.compute_per(d) >= 0.85 - 1e-12

        # Monotonic path loss & exact c propagation delay
        assert ch.path_loss_db(200.0) > ch.path_loss_db(50.0)
        assert ch.propagation_delay_s(300.0) == pytest.approx(300.0 / SPEED_OF_LIGHT_MPS)
        assert ch.transmission_time_s(1500) > ch.transmission_time_s(56)

    # Exercise Gilbert-Elliott Markov chain transitions over repeated draws
    ge_ch = WirelessChannel(
        ChannelConfig(
            base_p_loss=0.10,
            comm_range_m=350.0,
            p_good_to_bad=0.25,
            p_bad_to_good=0.25,
            seed=99,
        )
    )
    seen_bad = False
    for _ in range(100):
        _ = ge_ch.sample_deliveries([120.0, 180.0])
        if ge_ch.in_bad_state:
            seen_bad = True
            break
    assert seen_bad
