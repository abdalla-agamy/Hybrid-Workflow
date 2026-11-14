"""Stochastic UAV wireless channel with log-distance path loss, shadowing, and Gilbert-Elliott bursts."""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

SPEED_OF_LIGHT_MPS = 3.0e8


@dataclass(frozen=True)
class ChannelConfig:
    base_p_loss: float = 0.05
    comm_range_m: float = 350.0
    data_rate_mbps: float = 6.0
    path_loss_exponent: float = 2.4
    ref_loss_db: float = 40.0
    shadowing_sigma_db: float = 3.0
    p_good_to_bad: float = 0.05
    p_bad_to_good: float = 0.35
    bad_loss_multiplier: float = 2.5
    seed: int = 42


class WirelessChannel:
    """Models distance-dependent PER, log-distance path loss, shadowing, and burst errors."""

    def __init__(self, cfg: ChannelConfig | None = None) -> None:
        self.cfg = cfg if cfg is not None else ChannelConfig()
        self.rng = np.random.default_rng(self.cfg.seed)
        self.in_bad_state: bool = False

    def path_loss_db(self, distance_m: float, include_shadowing: bool = False) -> float:
        d = max(1.0, float(distance_m))
        pl = self.cfg.ref_loss_db + 10.0 * self.cfg.path_loss_exponent * math.log10(d)
        if include_shadowing and self.cfg.shadowing_sigma_db > 0.0:
            pl += float(self.rng.normal(0.0, self.cfg.shadowing_sigma_db))
        return pl

    def propagation_delay_s(self, distance_m: float) -> float:
        return max(0.0, float(distance_m)) / SPEED_OF_LIGHT_MPS

    def transmission_time_s(self, payload_bytes: int, phy_header_bytes: int = 24) -> float:
        total_bits = max(0, int(payload_bytes) + phy_header_bytes) * 8.0
        rate_bps = max(1e3, float(self.cfg.data_rate_mbps) * 1.0e6)
        return total_bits / rate_bps

    def compute_per(self, distance_m: float) -> float:
        """Compute expected packet error rate at distance_m."""
        per = self.compute_per_array(np.array([distance_m], dtype=np.float64))
        return float(per[0])

    def compute_per_array(self, distances_m: np.ndarray) -> np.ndarray:
        """Vectorized distance-dependent PER model across link distances."""
        d = np.maximum(0.0, np.asarray(distances_m, dtype=np.float64))
        r = max(1.0, float(self.cfg.comm_range_m))
        base = float(np.clip(self.cfg.base_p_loss, 0.0, 1.0))

        dist_per = np.empty_like(d)
        near_mask = d <= 50.0
        mid_mask = (~near_mask) & (d <= r)
        far_mask = d > r

        # Short range (d <= 50m): <= 0.01 physical fading loss so combined PER <= p_loss + 0.02
        dist_per[near_mask] = 0.01 * (d[near_mask] / 50.0) ** 2

        # Mid range (50m < d <= comm_range): smooth edge degradation up to ~0.12
        denom = max(1.0, r - 50.0)
        frac = (d[mid_mask] - 50.0) / denom
        dist_per[mid_mask] = 0.01 + 0.11 * (frac ** 2.2)

        # Combine baseline + physical fading on in-range links
        combined = 1.0 - (1.0 - base) * (1.0 - dist_per)

        # Out of range (d > comm_range): steep SNR cliff -> PER >= 0.85
        if np.any(far_mask):
            out_per = 0.85 + 0.14 * (1.0 - np.exp(-(d[far_mask] - r) / 40.0))
            combined[far_mask] = np.maximum(out_per, 0.85)

        return np.clip(combined, 0.0, 1.0)

    def _step_markov_state(self) -> float:
        """Advance Gilbert-Elliott 2-state Markov burst error chain."""
        if self.in_bad_state:
            if float(self.rng.random()) < self.cfg.p_bad_to_good:
                self.in_bad_state = False
        else:
            if float(self.rng.random()) < self.cfg.p_good_to_bad:
                self.in_bad_state = True
        return self.cfg.bad_loss_multiplier if self.in_bad_state else 1.0

    def sample_deliveries(self, distances_m: np.ndarray) -> np.ndarray:
        """Sample boolean delivery success for one fragment across an array of links."""
        d = np.asarray(distances_m, dtype=np.float64)
        base_per = self.compute_per_array(d)
        mult = self._step_markov_state()
        if mult != 1.0 and self.cfg.base_p_loss > 0.0:
            # Boost excess fading in BAD state, preserving deterministic zero loss at 0 distance when base=0
            eff_per = np.where(d > self.cfg.comm_range_m, base_per, np.minimum(0.95, base_per * mult))
        else:
            eff_per = base_per
        draws = self.rng.random(size=len(d))
        return draws >= eff_per

    def sample_deliveries_sequential(self, distances_m: np.ndarray) -> np.ndarray:
        """Vectorized PER sampling advancing Gilbert-Elliott burst state across a link sequence."""
        d = np.asarray(distances_m, dtype=np.float64)
        n = len(d)
        if n == 0:
            return np.empty(0, dtype=bool)
        base_per = self.compute_per_array(d)
        if self.cfg.base_p_loss > 0.0 and self.cfg.bad_loss_multiplier != 1.0:
            u = self.rng.random(size=n)
            bad_flags = np.empty(n, dtype=bool)
            in_bad = self.in_bad_state
            p_gb = self.cfg.p_good_to_bad
            p_bg = self.cfg.p_bad_to_good
            for i in range(n):
                if in_bad:
                    if u[i] < p_bg:
                        in_bad = False
                else:
                    if u[i] < p_gb:
                        in_bad = True
                bad_flags[i] = in_bad
            self.in_bad_state = in_bad
            mult = np.where(bad_flags, self.cfg.bad_loss_multiplier, 1.0)
            eff_per = np.where(
                d > self.cfg.comm_range_m,
                base_per,
                np.minimum(0.95, base_per * mult),
            )
        else:
            eff_per = base_per
        return self.rng.random(size=n) >= eff_per
