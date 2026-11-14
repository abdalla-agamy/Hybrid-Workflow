"""Unit tests for Phase 3B: spatial CSMA/CA MAC, MTU fragmentation, and 5-protocol replay."""

from __future__ import annotations

from pathlib import Path

import pytest
import simpy

from src.protocol_replay_agents import SimulationConfig, replay_trace
from src.radio_mac import MacConfig, RadioMAC, split_mtu_fragments
from src.topology_manager import TopologyManager
from src.trace_loader import (
    CryptoCost,
    InitialNode,
    SwarmTrace,
    TraceMeta,
    load_trace,
)
from src.wireless_channel import ChannelConfig, WirelessChannel

HYBRID_ROOT = Path(__file__).resolve().parents[2]
SAMPLES_DIR = HYBRID_ROOT / "traces" / "samples"


def _synthetic_trace(coords: list[tuple[int, int, str, float, float]]) -> SwarmTrace:
    meta = TraceMeta(
        seed=42,
        swarm_size=len(coords),
        num_clusters=1,
        protocol="HSBP",
        simulation_time=10.0,
        time_step=1.0,
        arena_width_m=1000.0,
        arena_height_m=1000.0,
        max_velocity_mps=20.0,
        key_size=32,
        header_size=16,
        control_size=8,
        message_size=56,
        packet_size=256,
        mtu=1500,
        generated_by="test",
        generated_at="2026-11-14T00:00:00Z",
    )
    nodes = tuple(
        InitialNode(
            uav_id=uid,
            cluster_id=cid,
            role=role,
            x=x,
            y=y,
            vx=0.0,
            vy=0.0,
            energy=100.0,
        )
        for uid, cid, role, x, y in coords
    )
    return SwarmTrace(meta=meta, initial_nodes=nodes, events=())


def test_mtu_fragmentation_splitting() -> None:
    assert split_mtu_fragments(0, 1500) == []
    assert split_mtu_fragments(56, 1500) == [56]
    assert split_mtu_fragments(2800, 1500) == [1500, 1300]
    frags_large = split_mtu_fragments(11200, 1500)
    assert len(frags_large) == 8
    assert sum(frags_large) == 11200
    assert all(f <= 1500 for f in frags_large)


def test_unicast_mtu_drop_aborts_message() -> None:
    trace = _synthetic_trace([
        (1, 1, "SL", 100.0, 100.0),
        (2, 1, "follower", 120.0, 100.0),
    ])
    env = simpy.Environment()
    topo = TopologyManager(trace)
    ch = WirelessChannel(ChannelConfig(base_p_loss=1.0, comm_range_m=350.0, seed=1))
    mac = RadioMAC(env, ch, topo, MacConfig(mtu_bytes=1500, max_retries=3, seed=2))

    proc = env.process(mac.transmit_unicast(1, 2, 3000))
    env.run(until=proc)
    stats = proc.value

    assert stats.delivered_count == 0
    assert stats.fragments_sent == 4  # First fragment exhausts 1 + 3 retries, second fragment aborted
    assert stats.retransmissions == 3


def test_broadcast_fragment_retry_and_out_of_range_drop() -> None:
    trace = _synthetic_trace([
        (1, 1, "SL", 0.0, 0.0),
        (2, 1, "follower", 20.0, 0.0),
        (3, 1, "follower", 30.0, 0.0),
        (4, 1, "follower", 900.0, 900.0),  # Way outside 350 m range
    ])
    env = simpy.Environment()
    topo = TopologyManager(trace)
    ch = WirelessChannel(ChannelConfig(base_p_loss=0.0, comm_range_m=350.0, seed=3))
    mac = RadioMAC(env, ch, topo, MacConfig(mtu_bytes=1500, max_retries=2, seed=4))

    proc = env.process(mac.transmit_broadcast(1, [2, 3, 4], 4000))  # 3 MTU fragments
    env.run(until=proc)
    stats = proc.value

    assert stats.target_count == 3
    assert bool(stats.delivered_mask[0]) is True
    assert bool(stats.delivered_mask[1]) is True
    assert bool(stats.delivered_mask[2]) is False
    assert stats.delivered_count == 2


def test_spatial_carrier_sense_serialization() -> None:
    trace = _synthetic_trace([
        (1, 1, "SL", 100.0, 100.0),
        (2, 1, "follower", 120.0, 100.0),
        (3, 1, "follower", 200.0, 100.0),
        (4, 1, "follower", 220.0, 100.0),
    ])
    env = simpy.Environment()
    topo = TopologyManager(trace)
    ch = WirelessChannel(ChannelConfig(base_p_loss=0.0, comm_range_m=350.0, data_rate_mbps=6.0, seed=5))
    mac = RadioMAC(env, ch, topo, MacConfig(carrier_sense_range_m=700.0, seed=6))

    p1 = env.process(mac.transmit_unicast(1, 2, 1500))
    p2 = env.process(mac.transmit_unicast(3, 4, 1500))
    env.run()
    s1 = p1.value
    s2 = p2.value

    assert s1.delivered_count == 1
    assert s2.delivered_count == 1
    # Because node 3 (x=200) is within 700m CS range of node 1 (x=100), transmissions serialize in time
    single_frame_min_s = ch.transmission_time_s(1528) + ch.transmission_time_s(14, phy_header_bytes=12)
    assert env.now >= 2.0 * single_frame_min_s
    assert max(s1.airtime_s, s2.airtime_s) >= 2.0 * single_frame_min_s


def test_all_five_protocols_replay_sample_traces() -> None:
    results = {}
    for proto in ["FlatSBP", "HSBP", "DTHSBP", "LKH", "TGDH"]:
        trace = load_trace(SAMPLES_DIR / f"events_trace_{proto}_N50.json", validate=True)
        summary, events = replay_trace(
            trace,
            SimulationConfig(p_loss=0.05, data_rate_mbps=6.0, comm_range_m=350.0, seed=42),
        )
        results[proto] = summary
        assert summary.rekey_events > 0
        assert summary.total_bytes_on_air > 0
        assert 0.0 < summary.mean_pdr <= 1.0
        assert 0.0 < summary.mean_key_sync_ratio <= 1.0
        assert len(events) == len(trace.events)
        for ev in events:
            assert ev.rekey_completion_latency_ms == pytest.approx(
                ev.crypto_latency_ms + ev.network_latency_ms
            )

    assert results["DTHSBP"].total_bytes_on_air < results["FlatSBP"].total_bytes_on_air
    assert results["DTHSBP"].total_bytes_on_air <= results["HSBP"].total_bytes_on_air
    assert results["DTHSBP"].collateral_leaves_batched >= 1
    assert results["DTHSBP"].dt_prediction_hit_rate > 0.0
