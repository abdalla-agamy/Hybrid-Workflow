"""CSMA/CA MAC layer with spatial carrier sensing, binary exponential backoff, and 1500B MTU fragmentation."""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np
import simpy

from .topology_manager import TopologyManager
from .wireless_channel import WirelessChannel


@dataclass(frozen=True)
class MacConfig:
    mtu_bytes: int = 1500
    mac_header_bytes: int = 28
    ack_bytes: int = 14
    ack_timeout_s: float = 0.0005
    sifs_s: float = 16e-6
    difs_s: float = 34e-6
    slot_time_s: float = 9e-6
    cw_min: int = 15
    cw_max: int = 1023
    max_retries: int = 3
    carrier_sense_range_m: float = 700.0
    seed: int = 101


@dataclass
class MacTxStats:
    fragments_sent: int = 0
    retransmissions: int = 0
    mac_collisions: int = 0
    bytes_on_air: int = 0
    airtime_s: float = 0.0
    delivered_count: int = 0
    target_count: int = 0
    delivered_mask: np.ndarray = field(default_factory=lambda: np.empty(0, dtype=bool))


def split_mtu_fragments(payload_bytes: int, mtu_bytes: int) -> list[int]:
    """Split payload_bytes into MTU-bounded frame payload sizes."""
    total = max(0, int(payload_bytes))
    mtu = max(1, int(mtu_bytes))
    if total == 0:
        return []
    n_full, rem = divmod(total, mtu)
    frags = [mtu] * n_full
    if rem > 0:
        frags.append(rem)
    return frags


class RadioMAC:
    """SimPy IEEE 802.11p-style spatial CSMA/CA MAC model."""

    def __init__(
        self,
        env: simpy.Environment,
        channel: WirelessChannel,
        topology: TopologyManager,
        cfg: MacConfig | None = None,
    ) -> None:
        self.env = env
        self.channel = channel
        self.topology = topology
        self.cfg = cfg if cfg is not None else MacConfig()
        self.rng = np.random.default_rng(self.cfg.seed)
        self.active_tx: list[tuple[int, float, float, float]] = []
        self._neighbor_cache_version: int = -1
        self._cs_neighbor_counts: dict[int, int] = {}

    def invalidate_neighbor_cache(self) -> None:
        self._neighbor_cache_version += 1
        self._cs_neighbor_counts.clear()

    def _get_cs_neighbors(self, sender_id: int) -> int:
        cached = self._cs_neighbor_counts.get(sender_id)
        if cached is not None:
            return cached
        ids, coords = self.topology.get_active_positions_array()
        if len(ids) <= 1 or sender_id not in self.topology.nodes:
            self._cs_neighbor_counts[sender_id] = 0
            return 0
        sn = self.topology.nodes[sender_id]
        dists = np.hypot(coords[:, 0] - sn.x, coords[:, 1] - sn.y)
        count = int(np.sum((ids != sender_id) & (dists <= self.cfg.carrier_sense_range_m)))
        self._cs_neighbor_counts[sender_id] = count
        return count

    def _compute_backoff_s(self, cw: int) -> float:
        slots = int(self.rng.integers(0, cw + 1))
        return slots * self.cfg.slot_time_s

    def _ambient_collision(self, sender_id: int, frame_duration_s: float) -> bool:
        """Model spatial hidden-terminal/contention probability inside carrier_sense_range_m."""
        n_cs = self._get_cs_neighbors(sender_id)
        if n_cs <= 0:
            return False
        # Scaled supervisory/neighbor contention load proportional to airtime and neighbor count
        n_eff = min(n_cs, 250)
        p_col = 1.0 - math.exp(-0.04 * n_eff * frame_duration_s)
        p_col = min(0.25, max(0.0, p_col))
        return bool(float(self.rng.random()) < p_col)

    def _is_medium_busy(self, sx: float, sy: float) -> float:
        """Return latest interfering end time (> now), or 0.0 if clear."""
        now = self.env.now
        self.active_tx = [tx for tx in self.active_tx if tx[3] > now]
        blocking_until = 0.0
        r_cs = self.cfg.carrier_sense_range_m
        for _, ox, oy, end_t in self.active_tx:
            if math.hypot(sx - ox, sy - oy) <= r_cs and end_t > blocking_until:
                blocking_until = end_t
        return blocking_until

    def _acquire_medium_csma(self, sx: float, sy: float, cw: int):
        """CSMA/CA wait-sense-backoff loop re-checking medium after DIFS + backoff."""
        while True:
            blocking_until = self._is_medium_busy(sx, sy)
            while blocking_until > self.env.now:
                yield self.env.timeout(blocking_until - self.env.now)
                blocking_until = self._is_medium_busy(sx, sy)

            backoff_s = self._compute_backoff_s(cw)
            yield self.env.timeout(self.cfg.difs_s + backoff_s)

            if self._is_medium_busy(sx, sy) <= self.env.now:
                break

    def transmit_unicast(self, sender_id: int, receiver_id: int, payload_bytes: int):
        """SimPy generator transmitting payload_bytes unicast with CSMA/CA, MTU fragmentation, and ACK retries."""
        stats = MacTxStats(target_count=1, delivered_mask=np.array([False], dtype=bool))
        if payload_bytes <= 0:
            stats.delivered_count = 1
            stats.delivered_mask = np.array([True], dtype=bool)
            return stats

        frags = split_mtu_fragments(payload_bytes, self.cfg.mtu_bytes)
        sx, sy = self.topology.get_position(sender_id)
        dist_m = self.topology.distance(sender_id, receiver_id)
        prop_s = self.channel.propagation_delay_s(dist_m)
        ack_s = self.channel.transmission_time_s(self.cfg.ack_bytes, phy_header_bytes=12)

        start_t = self.env.now
        dist_arr = np.array([dist_m], dtype=np.float64)

        for frag_bytes in frags:
            frame_bytes = frag_bytes + self.cfg.mac_header_bytes
            tx_s = self.channel.transmission_time_s(frame_bytes)
            cw = self.cfg.cw_min
            frag_ok = False

            for attempt in range(self.cfg.max_retries + 1):
                yield from self._acquire_medium_csma(sx, sy, cw)

                tx_end = self.env.now + tx_s + prop_s
                self.active_tx.append((sender_id, sx, sy, tx_end))
                stats.fragments_sent += 1
                stats.bytes_on_air += frame_bytes

                yield self.env.timeout(tx_s + prop_s)

                collided = self._ambient_collision(sender_id, tx_s)
                if collided:
                    stats.mac_collisions += 1
                    stats.retransmissions += 1 if attempt < self.cfg.max_retries else 0
                    yield self.env.timeout(self.cfg.ack_timeout_s)
                    cw = min(self.cfg.cw_max, 2 * cw + 1)
                    continue

                delivered = bool(self.channel.sample_deliveries(dist_arr)[0])
                if delivered:
                    yield self.env.timeout(self.cfg.sifs_s + ack_s + prop_s)
                    stats.bytes_on_air += self.cfg.ack_bytes
                    frag_ok = True
                    break

                if attempt < self.cfg.max_retries:
                    stats.retransmissions += 1
                    yield self.env.timeout(self.cfg.ack_timeout_s)
                    cw = min(self.cfg.cw_max, 2 * cw + 1)
                else:
                    yield self.env.timeout(self.cfg.ack_timeout_s)

            if not frag_ok:
                stats.airtime_s = self.env.now - start_t
                return stats

        stats.delivered_count = 1
        stats.delivered_mask = np.array([True], dtype=bool)
        stats.airtime_s = self.env.now - start_t
        return stats

    def transmit_broadcast(
        self,
        sender_id: int,
        receiver_ids: list[int] | np.ndarray,
        payload_bytes: int,
    ):
        """SimPy generator broadcasting MTU-fragmented payload to all receivers with vectorized PER & retries."""
        rx_ids = [int(r) for r in receiver_ids]
        n_rx = len(rx_ids)
        if n_rx == 0:
            return MacTxStats(target_count=0, delivered_count=0, delivered_mask=np.empty(0, dtype=bool))
        if payload_bytes <= 0:
            return MacTxStats(
                target_count=n_rx,
                delivered_count=n_rx,
                delivered_mask=np.ones(n_rx, dtype=bool),
            )

        stats = MacTxStats(
            target_count=n_rx,
            delivered_mask=np.ones(n_rx, dtype=bool),
        )
        frags = split_mtu_fragments(payload_bytes, self.cfg.mtu_bytes)
        sx, sy = self.topology.get_position(sender_id)

        d_arr = np.array(
            [self.topology.distance(sender_id, rid) for rid in rx_ids],
            dtype=np.float64,
        )
        max_prop_s = self.channel.propagation_delay_s(float(np.max(d_arr)))
        start_t = self.env.now

        for frag_bytes in frags:
            frame_bytes = frag_bytes + self.cfg.mac_header_bytes
            tx_s = self.channel.transmission_time_s(frame_bytes)
            cw = self.cfg.cw_min
            frag_received = np.zeros(n_rx, dtype=bool)

            for attempt in range(self.cfg.max_retries + 1):
                yield from self._acquire_medium_csma(sx, sy, cw)

                tx_end = self.env.now + tx_s + max_prop_s
                self.active_tx.append((sender_id, sx, sy, tx_end))
                stats.fragments_sent += 1
                stats.bytes_on_air += frame_bytes

                yield self.env.timeout(tx_s + max_prop_s + self.cfg.sifs_s)

                collided = self._ambient_collision(sender_id, tx_s)
                if collided:
                    stats.mac_collisions += 1
                    if attempt < self.cfg.max_retries:
                        stats.retransmissions += 1
                        cw = min(self.cfg.cw_max, 2 * cw + 1)
                    continue

                delivered_now = self.channel.sample_deliveries(d_arr)
                frag_received |= delivered_now
                if bool(np.all(frag_received)):
                    break

                if attempt < self.cfg.max_retries:
                    stats.retransmissions += 1
                    cw = min(self.cfg.cw_max, 2 * cw + 1)

            stats.delivered_mask &= frag_received

        stats.delivered_count = int(np.sum(stats.delivered_mask))
        stats.airtime_s = self.env.now - start_t
        return stats
