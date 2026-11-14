"""Dynamic 2D UAV topology manager with bounded nodePositions carry-forward."""

from __future__ import annotations

import math
from dataclasses import dataclass

import networkx as nx
import numpy as np
from scipy.sparse.csgraph import breadth_first_order

from .trace_loader import SwarmTrace, TraceEvent


@dataclass
class NodeState:
    uav_id: int
    cluster_id: int
    role: str
    x: float
    y: float
    active: bool = True


class TopologyManager:
    """Tracks live 2D coordinates, cluster memberships, CH/SL roles, and NetworkX graph.

    Positions are seeded from `initialNodes` and updated per event using bounded
    `ev.node_positions` snapshots, carrying forward the last known coordinates of
    UAVs in uninvolved clusters without losing global spatial state.
    """

    def __init__(self, trace: SwarmTrace) -> None:
        self.meta = trace.meta
        self.nodes: dict[int, NodeState] = {}
        self.cluster_members: dict[int, list[int]] = {
            c: [] for c in range(1, trace.meta.num_clusters + 1)
        }
        self.cluster_heads: dict[int, int | None] = {
            c: None for c in range(1, trace.meta.num_clusters + 1)
        }
        self.sl_id: int | None = None

        for init in trace.initial_nodes:
            state = NodeState(
                uav_id=init.uav_id,
                cluster_id=init.cluster_id,
                role=init.role,
                x=init.x,
                y=init.y,
                active=True,
            )
            self.nodes[init.uav_id] = state
            self.cluster_members.setdefault(init.cluster_id, []).append(init.uav_id)
            if init.role == "SL":
                self.sl_id = init.uav_id
                self.cluster_heads[init.cluster_id] = init.uav_id
            elif init.role == "CH":
                self.cluster_heads[init.cluster_id] = init.uav_id

    def apply_event(self, ev: TraceEvent) -> None:
        """Apply membership transitions and bounded coordinate updates for one event."""
        cid = ev.cluster_id
        self.cluster_members.setdefault(cid, [])

        if ev.type in {"leave", "failure", "dtPredictedDeparture"}:
            for uid in ev.affected_uavs:
                if uid in self.nodes:
                    self.nodes[uid].active = False
        elif ev.type == "join":
            for uid in ev.affected_uavs:
                if uid not in self.nodes:
                    self.nodes[uid] = NodeState(
                        uav_id=uid,
                        cluster_id=cid,
                        role="follower",
                        x=0.0,
                        y=0.0,
                        active=True,
                    )
                else:
                    self.nodes[uid].active = True
                    self.nodes[uid].cluster_id = cid

        # Carry forward all other nodes; update SL + target cluster positions from bounded snapshot.
        snapshot_roles_in_cid: dict[int, str] = {}
        for snap in ev.node_positions:
            if snap.uav_id in self.nodes:
                node = self.nodes[snap.uav_id]
                node.x = snap.x
                node.y = snap.y
                node.role = snap.role
                node.cluster_id = snap.cluster_id
                node.active = True
            else:
                self.nodes[snap.uav_id] = NodeState(
                    uav_id=snap.uav_id,
                    cluster_id=snap.cluster_id,
                    role=snap.role,
                    x=snap.x,
                    y=snap.y,
                    active=True,
                )

            if snap.role == "SL":
                self.sl_id = snap.uav_id
                self.cluster_heads[snap.cluster_id] = snap.uav_id
            elif snap.role == "CH":
                self.cluster_heads[snap.cluster_id] = snap.uav_id

            if snap.cluster_id == cid:
                snapshot_roles_in_cid[snap.uav_id] = snap.role

        # Sync exact post-event membership list for target cluster.
        new_members = list(ev.cluster_membership_after)
        new_set = set(new_members)
        for uid in self.cluster_members.get(cid, []):
            if uid not in new_set and uid in self.nodes:
                self.nodes[uid].active = False
        self.cluster_members[cid] = new_members

        # Reconcile CH and SL roles cleanly (supports CH departure reassignment).
        if not new_members:
            self.cluster_heads[cid] = None
        else:
            tagged_heads = [
                uid for uid, role in snapshot_roles_in_cid.items()
                if uid in new_set and role in {"CH", "SL"}
            ]
            if tagged_heads:
                self.cluster_heads[cid] = tagged_heads[0]
            else:
                cur_ch = self.cluster_heads.get(cid)
                if cur_ch is None or cur_ch not in new_set:
                    new_ch = new_members[0]
                    self.cluster_heads[cid] = new_ch
                    self.nodes[new_ch].role = "SL" if (cid == 1 and (self.sl_id is None or not self.nodes[self.sl_id].active)) else "CH"

        if self.sl_id is None or not self.nodes[self.sl_id].active:
            c1_head = self.cluster_heads.get(1)
            if c1_head is not None and self.nodes[c1_head].active:
                self.sl_id = c1_head
                self.nodes[c1_head].role = "SL"
            else:
                self.sl_id = next(
                    (h for h in self.cluster_heads.values() if h is not None and self.nodes[h].active),
                    None,
                )
                if self.sl_id is not None:
                    self.nodes[self.sl_id].role = "SL"

    def get_position(self, uav_id: int) -> tuple[float, float]:
        node = self.nodes[uav_id]
        return (node.x, node.y)

    def distance(self, uav_a: int, uav_b: int) -> float:
        na = self.nodes[uav_a]
        nb = self.nodes[uav_b]
        return math.hypot(na.x - nb.x, na.y - nb.y)

    def get_sl_id(self) -> int | None:
        if self.sl_id is not None and self.nodes[self.sl_id].active:
            return self.sl_id
        return None

    def get_ch_id(self, cluster_id: int) -> int | None:
        ch = self.cluster_heads.get(cluster_id)
        if ch is not None and ch in self.nodes and self.nodes[ch].active:
            return ch
        return None

    def get_cluster_followers(self, cluster_id: int, exclude_id: int | None = None) -> list[int]:
        ch = self.get_ch_id(cluster_id)
        members = self.cluster_members.get(cluster_id, [])
        return [
            uid for uid in members
            if uid != ch and uid != exclude_id and uid in self.nodes and self.nodes[uid].active
        ]

    def get_all_active_ids(self) -> list[int]:
        return [uid for uid, n in self.nodes.items() if n.active]

    def get_active_positions_array(self) -> tuple[np.ndarray, np.ndarray]:
        active = [(uid, n.x, n.y) for uid, n in self.nodes.items() if n.active]
        if not active:
            return np.empty(0, dtype=np.int64), np.empty((0, 2), dtype=np.float64)
        ids = np.array([item[0] for item in active], dtype=np.int64)
        coords = np.array([[item[1], item[2]] for item in active], dtype=np.float64)
        return ids, coords

    def build_connectivity_graph(self, comm_range_m: float) -> nx.Graph:
        """Build undirected NetworkX graph connecting active UAV pairs within comm_range_m."""
        g = nx.Graph()
        ids, coords = self.get_active_positions_array()
        for uid in ids.tolist():
            node = self.nodes[uid]
            g.add_node(uid, cluster_id=node.cluster_id, role=node.role, pos=(node.x, node.y))

        n = len(ids)
        if n <= 1:
            return g

        diff = coords[:, None, :] - coords[None, :, :]
        dist_mat = np.hypot(diff[:, :, 0], diff[:, :, 1])
        i_idx, j_idx = np.where((np.triu(np.ones((n, n), dtype=bool), k=1)) & (dist_mat <= comm_range_m))
        g.add_weighted_edges_from(
            (int(ids[i]), int(ids[j]), float(dist_mat[i, j]))
            for i, j in zip(i_idx.tolist(), j_idx.tolist())
        )
        # Keep backward-compatible edge attribute alias `distance_m`
        for u, v, d in g.edges(data=True):
            d["distance_m"] = d.pop("weight")
        return g

    def compute_bfs_paths(
        self,
        src_id: int,
        comm_range_m: float,
        target_ids: list[int] | None = None,
    ) -> dict[int, list[int]]:
        """Fast vectorized BFS shortest paths within comm_range_m equivalent to NetworkX BFS."""
        ids, coords = self.get_active_positions_array()
        id_to_idx = {int(uid): idx for idx, uid in enumerate(ids.tolist())}
        if src_id not in id_to_idx:
            return {}
        src_idx = id_to_idx[src_id]

        # Fast-path: if all requested targets are 1 hop away, skip O(N^2) matrix construction
        if target_ids is not None:
            sx, sy = coords[src_idx, 0], coords[src_idx, 1]
            t_present = [tid for tid in target_ids if tid in id_to_idx]
            if all(
                math.hypot(coords[id_to_idx[tid], 0] - sx, coords[id_to_idx[tid], 1] - sy) <= comm_range_m
                for tid in t_present
            ):
                return {tid: ([src_id] if tid == src_id else [src_id, tid]) for tid in t_present}

        diff = coords[:, None, :] - coords[None, :, :]
        dist_sq = diff[:, :, 0] ** 2 + diff[:, :, 1] ** 2
        adj = (dist_sq <= comm_range_m * comm_range_m).astype(np.int8)
        np.fill_diagonal(adj, 0)

        _, preds = breadth_first_order(adj, src_idx, directed=False, return_predecessors=True)

        targets = target_ids if target_ids is not None else ids.tolist()
        paths: dict[int, list[int]] = {}
        ids_list = ids.tolist()
        for tid in targets:
            tidx = id_to_idx.get(tid)
            if tidx is None:
                continue
            if tidx == src_idx:
                paths[tid] = [src_id]
                continue
            if preds[tidx] == -9999:
                continue
            rev = [tid]
            cur = tidx
            while cur != src_idx and preds[cur] >= 0:
                cur = int(preds[cur])
                rev.append(ids_list[cur])
            if cur == src_idx:
                rev.reverse()
                paths[tid] = rev
        return paths
