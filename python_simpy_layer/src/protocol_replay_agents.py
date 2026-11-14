"""Protocol trace replay engine on top of SimPy RadioMAC, TopologyManager, and WirelessChannel."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Any

import networkx as nx
import numpy as np
import simpy

from .radio_mac import MacConfig, MacTxStats, RadioMAC
from .topology_manager import TopologyManager
from .trace_loader import HopDescriptor, SwarmTrace, TraceEvent
from .wireless_channel import ChannelConfig, WirelessChannel


@dataclass(frozen=True)
class SimulationConfig:
    p_loss: float = 0.05
    data_rate_mbps: float = 6.0
    comm_range_m: float = 350.0
    carrier_sense_range_m: float = 700.0
    mtu_bytes: int = 1500
    max_retries: int = 3
    seed: int = 42
    enable_multihop_relay: bool = True


@dataclass
class EventMetrics:
    protocol: str
    swarm_size: int
    p_loss: float
    data_rate_mbps: float
    t: float
    event_type: str
    cluster_id: int
    rekey_count: int
    is_collateral_batch: bool
    affected_count: int
    prediction_hit: bool | None
    crypto_bytes: int
    bytes_on_air: int
    fragments_sent: int
    retransmissions: int
    mac_collisions: int
    crypto_latency_ms: float
    network_latency_ms: float
    rekey_completion_latency_ms: float
    pdr: float
    key_sync_ratio: float


@dataclass
class SimulationSummary:
    protocol: str
    swarm_size: int
    p_loss: float
    data_rate_mbps: float
    comm_range_m: float
    num_events: int
    rekey_events: int
    dt_predicted_events: int
    collateral_batches: int
    collateral_leaves_batched: int
    dt_prediction_hit_rate: float
    total_crypto_bytes_matlab: int
    total_bytes_on_air: int
    fragments_sent: int
    retransmissions: int
    mac_collisions: int
    mean_crypto_latency_ms: float
    mean_network_latency_ms: float
    mean_rekey_completion_latency_ms: float
    p95_rekey_completion_latency_ms: float
    mean_pdr: float
    mean_key_sync_ratio: float

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class ProtocolReplayRunner:
    """Replays exact MATLAB event traces through SimPy wireless MAC + NetworkX topology."""

    def __init__(self, trace: SwarmTrace, cfg: SimulationConfig) -> None:
        self.trace = trace
        self.cfg = cfg
        self.env = simpy.Environment()
        self.topology = TopologyManager(trace)
        self.channel = WirelessChannel(
            ChannelConfig(
                base_p_loss=cfg.p_loss,
                comm_range_m=cfg.comm_range_m,
                data_rate_mbps=cfg.data_rate_mbps,
                seed=cfg.seed,
            )
        )
        self.mac = RadioMAC(
            self.env,
            self.channel,
            self.topology,
            MacConfig(
                mtu_bytes=max(64, int(cfg.mtu_bytes)),
                max_retries=cfg.max_retries,
                carrier_sense_range_m=cfg.carrier_sense_range_m,
                seed=cfg.seed + 17,
            ),
        )
        self.event_records: list[EventMetrics] = []
        self._graph_cache: nx.Graph | None = None

    def _get_graph(self) -> nx.Graph:
        if self._graph_cache is None:
            self._graph_cache = self.topology.build_connectivity_graph(self.cfg.comm_range_m)
        return self._graph_cache

    def _relay_unicast(
        self,
        src_id: int,
        dst_id: int,
        payload_bytes: int,
        shortest_paths: dict[int, list[int]] | None = None,
    ):
        """Transmit unicast using NetworkX shortest-path relay when dst exceeds comm_range_m."""
        agg = MacTxStats(target_count=1, delivered_mask=np.array([False], dtype=bool))
        if src_id == dst_id or payload_bytes <= 0:
            agg.delivered_count = 1
            agg.delivered_mask = np.array([True], dtype=bool)
            return agg

        dist_direct = self.topology.distance(src_id, dst_id)
        path: list[int] | None = None
        if dist_direct <= self.cfg.comm_range_m or not self.cfg.enable_multihop_relay:
            path = [src_id, dst_id]
        else:
            if shortest_paths is not None:
                path = shortest_paths.get(dst_id, [src_id, dst_id])
            else:
                bfs = self.topology.compute_bfs_paths(src_id, self.cfg.comm_range_m, [dst_id])
                path = bfs.get(dst_id, [src_id, dst_id])

        t0 = self.env.now
        for u, v in zip(path[:-1], path[1:]):
            hop_stats = yield from self.mac.transmit_unicast(u, v, payload_bytes)
            agg.fragments_sent += hop_stats.fragments_sent
            agg.retransmissions += hop_stats.retransmissions
            agg.mac_collisions += hop_stats.mac_collisions
            agg.bytes_on_air += hop_stats.bytes_on_air
            if hop_stats.delivered_count == 0:
                agg.airtime_s = self.env.now - t0
                return agg

        agg.delivered_count = 1
        agg.delivered_mask = np.array([True], dtype=bool)
        agg.airtime_s = self.env.now - t0
        return agg

    def _disseminate_cluster_broadcast(
        self,
        ch_id: int,
        follower_ids: list[int],
        payload_bytes: int,
    ):
        """Disseminate CH_TO_FOLLOWER round via 1-hop broadcast + BFS relays for out-of-range nodes."""
        n_targets = len(follower_ids)
        if n_targets == 0 or payload_bytes <= 0:
            return MacTxStats(
                target_count=n_targets,
                delivered_count=n_targets,
                delivered_mask=np.ones(n_targets, dtype=bool),
            )

        t0 = self.env.now
        agg = MacTxStats(target_count=n_targets, delivered_mask=np.zeros(n_targets, dtype=bool))
        idx_map = {fid: i for i, fid in enumerate(follower_ids)}

        in_range = [fid for fid in follower_ids if self.topology.distance(ch_id, fid) <= self.cfg.comm_range_m]
        out_range = [fid for fid in follower_ids if fid not in set(in_range)]

        # 1. Direct broadcast from CH (covering in-range followers directly; attempt long-range only if unrelayable)
        relay_paths: dict[int, list[int]] = {}
        unconnected: list[int] = []

        if out_range and self.cfg.enable_multihop_relay:
            relay_paths = self.topology.compute_bfs_paths(ch_id, self.cfg.comm_range_m, out_range)
            for fid in out_range:
                if fid not in relay_paths:
                    unconnected.append(fid)
        else:
            unconnected = out_range

        direct_batch = in_range + unconnected
        covered: set[int] = set()

        if direct_batch:
            st = yield from self.mac.transmit_broadcast(ch_id, direct_batch, payload_bytes)
            agg.fragments_sent += st.fragments_sent
            agg.retransmissions += st.retransmissions
            agg.mac_collisions += st.mac_collisions
            agg.bytes_on_air += st.bytes_on_air
            for fid, ok in zip(direct_batch, st.delivered_mask.tolist()):
                if ok:
                    agg.delivered_mask[idx_map[fid]] = True
                    covered.add(fid)

        # 2. Relay out-of-range cluster followers along BFS levels from covered nodes (including CH neighbors)
        if relay_paths:
            covered_frontier = {ch_id} | covered
            max_depth = max((len(p) - 1 for p in relay_paths.values()), default=0)
            for edge_idx in range(1, max_depth):
                # Group children receiving at hop `edge_idx -> edge_idx+1` by their parent relay node
                by_parent: dict[int, list[int]] = {}
                for fid, p in relay_paths.items():
                    if len(p) - 1 > edge_idx:
                        parent = p[edge_idx]
                        child = p[edge_idx + 1]
                        if parent in covered_frontier and child not in covered_frontier:
                            by_parent.setdefault(parent, []).append(child)

                for parent, children in by_parent.items():
                    uniq_children = list(dict.fromkeys(children))
                    st = yield from self.mac.transmit_broadcast(parent, uniq_children, payload_bytes)
                    agg.fragments_sent += st.fragments_sent
                    agg.retransmissions += st.retransmissions
                    agg.mac_collisions += st.mac_collisions
                    agg.bytes_on_air += st.bytes_on_air
                    for child, ok in zip(uniq_children, st.delivered_mask.tolist()):
                        if ok:
                            covered_frontier.add(child)
                            if child in idx_map:
                                agg.delivered_mask[idx_map[child]] = True

        agg.delivered_count = int(np.sum(agg.delivered_mask))
        agg.airtime_s = self.env.now - t0
        return agg

    def _process_event(self, ev: TraceEvent):
        self.topology.apply_event(ev)
        self._graph_cache = None
        self.mac.invalidate_neighbor_cache()

        t_net_start = self.env.now
        bytes_air = 0
        frags = 0
        retx = 0
        cols = 0

        sl_id = self.topology.get_sl_id()
        ch_id = self.topology.get_ch_id(ev.cluster_id)
        cluster_followers = self.topology.get_cluster_followers(ev.cluster_id)

        hop_pdrs: list[float] = []
        round_sync_masks: list[np.ndarray] = []

        # Fast path detection for FlatSBP sequential SL->follower broadcasts
        if ev.hops and all(h.kind == "CH_TO_FOLLOWER" and h.src_role == "SL" for h in ev.hops):
            if sl_id is not None:
                active_pool = [uid for uid in self.topology.get_all_active_ids() if uid != sl_id]
                if active_pool:
                    sl_paths = (
                        self.topology.compute_bfs_paths(sl_id, self.cfg.comm_range_m, active_pool)
                        if self.cfg.enable_multihop_relay
                        else {}
                    )
                    n_pool = len(active_pool)
                    target_ok: dict[int, bool] = {}
                    for idx, hop in enumerate(ev.hops):
                        dst = active_pool[idx % n_pool]
                        st = yield from self._relay_unicast(sl_id, dst, hop.bytes_len, sl_paths)
                        bytes_air += st.bytes_on_air
                        frags += st.fragments_sent
                        retx += st.retransmissions
                        cols += st.mac_collisions
                        ok = st.delivered_count > 0
                        hop_pdrs.append(1.0 if ok else 0.0)
                        target_ok[dst] = target_ok.get(dst, True) and ok
                    if target_ok:
                        key_sync = sum(1 for v in target_ok.values() if v) / len(target_ok)
                    else:
                        key_sync = 1.0
                else:
                    key_sync = 1.0
            else:
                key_sync = 1.0
        else:
            sl_to_ch_ok = True
            for hop in ev.hops:
                if hop.kind == "FOLLOWER_TO_CH_DT":
                    src = next(
                        (
                            uid
                            for uid in ev.affected_uavs
                            if uid in self.topology.nodes
                            and self.topology.nodes[uid].active
                            and uid != ch_id
                        ),
                        None,
                    )
                    if src is not None and ch_id is not None:
                        st = yield from self._relay_unicast(src, ch_id, hop.bytes_len)
                        bytes_air += st.bytes_on_air
                        frags += st.fragments_sent
                        retx += st.retransmissions
                        cols += st.mac_collisions
                        hop_pdrs.append(float(st.delivered_count))
                elif hop.kind == "SL_TO_CH":
                    if sl_id is not None and ch_id is not None and sl_id != ch_id:
                        st = yield from self._relay_unicast(sl_id, ch_id, hop.bytes_len)
                        bytes_air += st.bytes_on_air
                        frags += st.fragments_sent
                        retx += st.retransmissions
                        cols += st.mac_collisions
                        sl_to_ch_ok = st.delivered_count > 0
                        hop_pdrs.append(1.0 if sl_to_ch_ok else 0.0)
                    else:
                        # SL doubles as Cluster 1 CH: local loopback without RF airtime
                        hop_pdrs.append(1.0)
                elif hop.kind == "CH_TO_FOLLOWER":
                    if ch_id is not None and cluster_followers:
                        if not sl_to_ch_ok:
                            # If SL_TO_CH failed completely, CH cannot disseminate the new key round
                            round_sync_masks.append(np.zeros(len(cluster_followers), dtype=bool))
                            hop_pdrs.append(0.0)
                        else:
                            st = yield from self._disseminate_cluster_broadcast(
                                ch_id, cluster_followers, hop.bytes_len
                            )
                            bytes_air += st.bytes_on_air
                            frags += st.fragments_sent
                            retx += st.retransmissions
                            cols += st.mac_collisions
                            pdr_h = st.delivered_count / st.target_count if st.target_count > 0 else 1.0
                            hop_pdrs.append(pdr_h)
                            round_sync_masks.append(st.delivered_mask)
                    else:
                        hop_pdrs.append(1.0)

            if round_sync_masks:
                combined_sync = round_sync_masks[0].copy()
                for mask in round_sync_masks[1:]:
                    combined_sync &= mask
                key_sync = float(np.mean(combined_sync))
            elif ev.crypto.rekey_count > 0:
                key_sync = 1.0 if sl_to_ch_ok else 0.0
            else:
                key_sync = 1.0

        net_ms = (self.env.now - t_net_start) * 1000.0
        crypto_ms = ev.crypto.leader_time_ms
        mean_pdr = float(np.mean(hop_pdrs)) if hop_pdrs else 1.0

        self.event_records.append(
            EventMetrics(
                protocol=self.trace.meta.protocol,
                swarm_size=self.trace.meta.swarm_size,
                p_loss=self.cfg.p_loss,
                data_rate_mbps=self.cfg.data_rate_mbps,
                t=ev.t,
                event_type=ev.type,
                cluster_id=ev.cluster_id,
                rekey_count=ev.crypto.rekey_count,
                is_collateral_batch=ev.is_collateral_batch,
                affected_count=len(ev.affected_uavs),
                prediction_hit=ev.prediction_hit,
                crypto_bytes=ev.crypto.bytes_transmitted,
                bytes_on_air=bytes_air,
                fragments_sent=frags,
                retransmissions=retx,
                mac_collisions=cols,
                crypto_latency_ms=crypto_ms,
                network_latency_ms=net_ms,
                rekey_completion_latency_ms=crypto_ms + net_ms,
                pdr=mean_pdr,
                key_sync_ratio=key_sync,
            )
        )

    def _driver_process(self):
        for ev in self.trace.events:
            if ev.t > self.env.now:
                yield self.env.timeout(ev.t - self.env.now)
            yield from self._process_event(ev)

    def run(self) -> tuple[SimulationSummary, list[EventMetrics]]:
        self.env.process(self._driver_process())
        self.env.run()
        return self.summarize(), self.event_records

    def summarize(self) -> SimulationSummary:
        rekeys = [r for r in self.event_records if r.rekey_count > 0]
        dt_events = [r for r in rekeys if r.prediction_hit is not None]
        dt_hits = [r for r in dt_events if r.prediction_hit is True]
        batches = [r for r in rekeys if r.is_collateral_batch]
        batched_leaves = sum(max(0, r.affected_count - 1) for r in batches)

        if rekeys:
            crypto_arr = np.array([r.crypto_latency_ms for r in rekeys], dtype=np.float64)
            net_arr = np.array([r.network_latency_ms for r in rekeys], dtype=np.float64)
            tot_arr = np.array([r.rekey_completion_latency_ms for r in rekeys], dtype=np.float64)
            pdr_arr = np.array([r.pdr for r in rekeys], dtype=np.float64)
            sync_arr = np.array([r.key_sync_ratio for r in rekeys], dtype=np.float64)
            mean_crypto = float(np.mean(crypto_arr))
            mean_net = float(np.mean(net_arr))
            mean_tot = float(np.mean(tot_arr))
            p95_tot = float(np.percentile(tot_arr, 95))
            mean_pdr = float(np.mean(pdr_arr))
            mean_sync = float(np.mean(sync_arr))
        else:
            mean_crypto = mean_net = mean_tot = p95_tot = 0.0
            mean_pdr = mean_sync = 1.0

        hit_rate = len(dt_hits) / len(dt_events) if dt_events else 0.0

        return SimulationSummary(
            protocol=self.trace.meta.protocol,
            swarm_size=self.trace.meta.swarm_size,
            p_loss=self.cfg.p_loss,
            data_rate_mbps=self.cfg.data_rate_mbps,
            comm_range_m=self.cfg.comm_range_m,
            num_events=len(self.event_records),
            rekey_events=len(rekeys),
            dt_predicted_events=sum(1 for r in self.event_records if r.event_type == "dtPredictedDeparture"),
            collateral_batches=len(batches),
            collateral_leaves_batched=batched_leaves,
            dt_prediction_hit_rate=hit_rate,
            total_crypto_bytes_matlab=sum(r.crypto_bytes for r in self.event_records),
            total_bytes_on_air=sum(r.bytes_on_air for r in self.event_records),
            fragments_sent=sum(r.fragments_sent for r in self.event_records),
            retransmissions=sum(r.retransmissions for r in self.event_records),
            mac_collisions=sum(r.mac_collisions for r in self.event_records),
            mean_crypto_latency_ms=mean_crypto,
            mean_network_latency_ms=mean_net,
            mean_rekey_completion_latency_ms=mean_tot,
            p95_rekey_completion_latency_ms=p95_tot,
            mean_pdr=mean_pdr,
            mean_key_sync_ratio=mean_sync,
        )


def replay_trace(trace: SwarmTrace, cfg: SimulationConfig | None = None) -> tuple[SimulationSummary, list[EventMetrics]]:
    """Convenience entry point replaying a SwarmTrace under SimPy."""
    sim_cfg = cfg if cfg is not None else SimulationConfig()
    return ProtocolReplayRunner(trace, sim_cfg).run()
