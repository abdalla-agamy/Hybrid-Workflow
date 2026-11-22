# Phase 4 Comparative Technical Report: MATLAB Analytical Baseline vs. SimPy CSMA/CA Network Replay

**Generated:** 2026-11-22 16:54:57  
**Baseline Dataset:** `baseline_matlab_results/matlab_baseline_metrics.json` (`runCompleteEvaluationAndReport.m`, generated `2026-11-14T18:43:00`)  
**Network Replay Sweep:** `200` runs (`20824` total trace events across `FlatSBP`, `HSBP`, `DTHSBP`, `LKH`, `TGDH`), `seed=42`, `comm_range=350.0 m`, `carrier_sense=700.0 m`, `MTU=1500 B`, `max_retries=3`

---

## 1. Executive Summary & The System Evaluation Boundary

In the original MATLAB DES (`./DT-HSBP`), rekey communication is evaluated purely analytically: messages consume zero airtime, suffer `0%` packet loss, trigger zero CSMA/CA backoff collisions, and traverse multi-hop topologies at infinite bandwidth. By exporting deterministic **paired event traces** (identical exogenous Poisson arrivals, continuous specular-reflected 2D trajectories, and synchronized DT hazard draws) and replaying all five protocols over our Phase 3 SimPy wireless stack (log-distance shadowing + Gilbert-Elliott bursty PER + `700 m` spatial CSMA/CA + `1500 B` MTU fragmentation + BFS multi-hop relay), this report quantifies the exact gap across the **system evaluation boundary**:

1. **Multi-Hop + MAC Traffic Amplification (`4.5x – 30.7x`)**: At `N = 1,000` (`6 Mbps`, `p_loss = 0.15`), actual on-air bytes are **`4.5x`** higher than MATLAB's analytical byte count for `FlatSBP`, **`23.2x`** for `HSBP`, **`22.5x`** for `DTHSBP`, **`30.7x`** for `LKH`, and **`28.7x`** for `TGDH`.
2. **Network Delay Dominates Crypto Latency (`> 95%` at Scale)**: At `N = 250` (`p_loss = 0.05`, `6 Mbps`), wireless CSMA/CA serialization, backoff, and multi-hop forwarding account for **`99.31%`** of `DTHSBP` completion latency (`32.91 ms` vs. `0.230 ms` crypto time) and **`99.27%`** of `FlatSBP` latency (`226.42 ms`).
3. **End-to-End `DT-HSBP` Advantage Persists Under Contention & Loss**: Across all **40** grid points (`N in {50..1000}`, `p_loss in {0.0..0.15}`, `6` & `54 Mbps`), `DT-HSBP` slashes on-air bytes by **`42.7%–82.3%`** and end-to-end latency by **`79.0%–95.8%`** relative to `Flat-SBP`, while outperforming reactive `HSBP` across every dense swarm point (`N >= 100`) — saving up to **`45.4%`** air traffic at `N = 1000` (`p=0.15`).
4. **Rate-Dependent `LKH`/`TGDH` vs. `DT-HSBP` Crossover**: Because joins use `nc * 56 B` (`2,800 B` at `N=250`, `11,200 B` at `N=1,000`) while `DT-HSBP` lifespan-sorted tail departures truncate to `56 B` (`eta = 0.85`) and `LKH`/`TGDH` send compact `48 B` / `128 B` `O(log N)` messages across tree rounds, high-rate `54 Mbps` links reveal multi-round tree handshake penalties (`LKH` & `TGDH` slower than `DT-HSBP` at `N <= 100`, and `TGDH` slower through `N = 250`), whereas slow `6 Mbps` serialization at `N = 1,000` still reflects join/cascade payload differences.

---

## 2. MATLAB Baseline Reference (`N = 1,000..5,000`)

From `baseline_matlab_results/matlab_baseline_metrics.json`:
- **Section 1 (`eta = 0.85`)**: Across `N = 1,000..5,000`, `DT-HSBP` cuts analytical leave bytes by **`55.9%–55.3%`** vs. `HSBP` and **`91.0%–91.3%`** vs. `Flat-SBP`.
- **Section 4 (`N = 3,000, nc = 600`)**: `Flat-SBP` requires `479,775.2 B` (`182.66 ms`), `HSBP` requires `95,705.4 B` (`54.91 ms`), `LKH (d=3)` uses `2,592 B` (`1.69 ms`), `TGDH` uses `3,840 B` (`6.38 ms`), and full proposed `DT-HSBP + Contrib A + Contrib B` achieves `42,779.8 B` with only `4.14 ms` leader compute time (**91.1%** comm reduction and **97.7%** compute latency reduction vs. `Flat-SBP`).
- **Section 6 (`M = 1..25`, `nc = 50`)**: Under batched concurrent departures, `DT-HSBP` scales sub-linearly while reactive `HSBP` grows to `236,800 B` (`97.30%` analytical batch saving at `M = 25`).

---

## 3. MATLAB Analytical vs. SimPy Wireless Replay Quantitative Comparison (`p_loss = 0.05`)

| Swarm | Bitrate | Protocol | MATLAB Crypto (B) | SimPy Air Bytes (Amp) | Crypto Latency (ms) | Network Latency (ms) | Total Latency (ms) | Mean PDR | Key Sync |
|---:|:---:|:---|---:|---:|---:|---:|---:|---:|---:|
| N=50 | 6 Mbps | Flat-SBP | 298,984 | 1,314,802 (4.4x) | 0.598 | 63.51 | 64.10 | 90.23% | 90.22% |
| N=50 | 6 Mbps | HSBP | 61,040 | 402,596 (6.6x) | 0.148 | 6.98 | 7.13 | 93.14% | 92.22% |
| N=50 | 6 Mbps | DT-HSBP | 37,520 | 246,126 (6.6x) | 0.115 | 5.09 | 5.21 | 90.88% | 90.01% |
| N=50 | 6 Mbps | LKH (d=3) | 30,768 | 257,752 (8.4x) | 0.048 | 7.82 | 7.87 | 92.71% | 91.71% |
| N=50 | 6 Mbps | TGDH | 51,072 | 392,712 (7.7x) | 0.154 | 10.04 | 10.19 | 90.56% | 89.76% |
| N=50 | 54 Mbps | Flat-SBP | 298,984 | 1,314,544 (4.4x) | 0.598 | 42.68 | 43.27 | 90.20% | 90.18% |
| N=50 | 54 Mbps | HSBP | 61,040 | 402,064 (6.6x) | 0.148 | 2.00 | 2.15 | 93.14% | 92.22% |
| N=50 | 54 Mbps | DT-HSBP | 37,520 | 246,126 (6.6x) | 0.115 | 1.85 | 1.97 | 90.88% | 90.01% |
| N=50 | 54 Mbps | LKH (d=3) | 30,768 | 257,676 (8.4x) | 0.048 | 4.11 | 4.16 | 92.71% | 91.71% |
| N=50 | 54 Mbps | TGDH | 51,072 | 392,712 (7.7x) | 0.154 | 4.72 | 4.87 | 90.56% | 89.76% |
| N=250 | 6 Mbps | Flat-SBP | 1,430,184 | 5,740,398 (4.0x) | 1.669 | 226.42 | 228.09 | 99.86% | 99.86% |
| N=250 | 6 Mbps | HSBP | 289,800 | 3,490,050 (12.0x) | 0.342 | 51.69 | 52.03 | 99.86% | 99.72% |
| N=250 | 6 Mbps | DT-HSBP | 170,464 | 2,047,568 (12.0x) | 0.230 | 32.91 | 33.14 | 99.89% | 99.77% |
| N=250 | 6 Mbps | LKH (d=3) | 41,040 | 641,410 (15.6x) | 0.011 | 19.26 | 19.27 | 99.87% | 99.26% |
| N=250 | 6 Mbps | TGDH | 77,568 | 1,128,494 (14.5x) | 0.063 | 29.22 | 29.28 | 99.94% | 99.55% |
| N=250 | 54 Mbps | Flat-SBP | 1,430,184 | 5,732,698 (4.0x) | 1.669 | 133.96 | 135.62 | 99.91% | 99.91% |
| N=250 | 54 Mbps | HSBP | 289,800 | 3,485,780 (12.0x) | 0.342 | 10.00 | 10.35 | 98.88% | 98.76% |
| N=250 | 54 Mbps | DT-HSBP | 170,464 | 1,998,300 (11.7x) | 0.230 | 6.78 | 7.01 | 99.89% | 99.77% |
| N=250 | 54 Mbps | LKH (d=3) | 41,040 | 646,682 (15.8x) | 0.011 | 10.25 | 10.26 | 99.83% | 99.07% |
| N=250 | 54 Mbps | TGDH | 77,568 | 1,126,510 (14.5x) | 0.063 | 13.85 | 13.91 | 99.94% | 99.58% |
| N=1000 | 6 Mbps | Flat-SBP | 5,672,184 | 23,001,850 (4.1x) | 6.641 | 923.85 | 930.49 | 99.82% | 99.82% |
| N=1000 | 6 Mbps | HSBP | 1,133,272 | 22,306,042 (19.7x) | 3.128 | 328.99 | 332.12 | 99.46% | 98.92% |
| N=1000 | 6 Mbps | DT-HSBP | 636,552 | 12,333,526 (19.4x) | 1.259 | 191.02 | 192.28 | 99.10% | 98.19% |
| N=1000 | 6 Mbps | LKH (d=3) | 50,688 | 1,282,858 (25.3x) | 0.019 | 40.51 | 40.53 | 99.86% | 99.04% |
| N=1000 | 6 Mbps | TGDH | 103,424 | 2,443,614 (23.6x) | 0.164 | 65.96 | 66.13 | 99.69% | 97.18% |
| N=1000 | 54 Mbps | Flat-SBP | 5,672,184 | 22,977,210 (4.1x) | 6.641 | 555.66 | 562.30 | 99.82% | 99.82% |
| N=1000 | 54 Mbps | HSBP | 1,133,272 | 22,098,294 (19.5x) | 3.128 | 62.04 | 65.16 | 99.14% | 98.27% |
| N=1000 | 54 Mbps | DT-HSBP | 636,552 | 12,085,374 (19.0x) | 1.259 | 36.48 | 37.74 | 97.81% | 96.64% |
| N=1000 | 54 Mbps | LKH (d=3) | 50,688 | 1,283,454 (25.3x) | 0.019 | 22.28 | 22.30 | 99.86% | 99.04% |
| N=1000 | 54 Mbps | TGDH | 103,424 | 2,418,670 (23.4x) | 0.164 | 32.63 | 32.79 | 98.89% | 97.91% |

---

## 4. Comparative Figures & Cross-Layer Interpretation

### 4.1 Analytical Communication Cost vs. Multi-Hop Air Traffic
![Figure 1: Communication Cost MATLAB vs SimPy](fig1_communication_cost_matlab_vs_simpy.png)

MATLAB counts bytes only once at the logical key-server interface. Inside SimPy, unicast `Flat-SBP` and tree packets must traverse multi-hop BFS routes across cluster boundaries, while hierarchical broadcasts use depth-1 cluster-head dissemination plus greedy 2-hop relay sets and Gilbert-Elliott ARQ retransmissions. Consequently, at `N = 1,000` (`p_loss = 0.15, 6 Mbps`), on-air volume expands to **`25,173 KB`** (`FlatSBP`, `4.5x`), **`25,659 KB`** (`HSBP`, `23.2x`), **`14,006 KB`** (`DT-HSBP`, `22.5x`), **`1,518 KB`** (`LKH`, `30.7x`), and **`2,897 KB`** (`TGDH`, `28.7x`).

### 4.2 End-to-End Rekey Latency Decomposition
![Figure 2: Rekey Latency Breakdown](fig2_rekey_latency_breakdown.png)

Panel (a) isolates how misleading crypto-only latency benchmarks can be over shared wireless channels: at `N = 250, p_loss = 0.05, 6 Mbps`, `DT-HSBP`'s leader crypto computation (`0.230 ms`) is only `0.69%` of actual completion time (`33.14 ms`). On `54 Mbps` links (Panel (b)), where airtime serialization shrinks by `9x`, CSMA/CA DIFS/backoff rounds expose `LKH`/`TGDH`'s sequential tree rounds at `N = 50–100` (and `TGDH` through `N = 250`), whereas at `6 Mbps` for larger clusters (`N >= 250`), transmitting `nc * 56 B` cluster payloads shifts serialization cost above `LKH`/`TGDH`'s compact `O(log N)` packets.

### 4.3 Packet Delivery Ratio & Key Synchronization Under Bursty Loss
![Figure 3: PDR and Key Sync Ratio vs Channel Loss](fig3_pdr_and_keysync_vs_loss.png)

1. **Sparse vs. Dense Spatial Connectivity**: At `N = 50` in `1,000 x 1,000 m` (`comm_range = 350 m`), random UAV motion yields occasional boundary partitions, bounding zero-loss `mean_key_sync_ratio` to `0.901–0.921`. At denser scales (`N >= 100`), lossless `mean_key_sync_ratio` reaches **`99.8%–100.0%`** across all five protocols.
2. **Bursty Loss Resilience**: Moving from `p_loss = 0.0` to `0.15` at `N = 250` (`6 Mbps`) lowers `DT-HSBP` PDR from `99.99%` to `98.62%` and key sync ratio to `97.25%` (`91.26%` at `N = 1,000`), while triggering ARQ retries and higher channel busy times that cannot be observed in MATLAB.

### 4.4 Digital-Twin Collateral Batching & Lifespan Sorting Gains in Wireless Channels
![Figure 4: DT Batch Efficiency Under Loss](fig4_dt_batch_efficiency_under_loss.png)

In the 100-second Poisson traces (`N = 50..1,000`), `DT-HSBP` combines Contribution A lifespan-sorted `O(1)` tail truncation (`eta = 0.85`) with **4–5** consolidated collateral departure batches (`1424` predicted events across the 200-run sweep). Across all swarms (`N >= 50`), this reduces on-air bytes vs. reactive `HSBP` by **`32.0%–45.4%`** across the mixed-event window (including `326 -> 178` MAC collisions at `N = 1,000, p_loss = 0.05, 6 Mbps`). When evaluated purely on burst collateral leave episodes (`M = 25`, Panel (a)), savings scale to **`97.30%`**.

---

## 5. Readiness for Phase 5 (OMNeT++ 6.x / INET 4.5 Packet-Level Cross-Validation)

While SimPy accurately models 802.11-style spatial carrier sensing, Gilbert-Elliott PER, MTU fragmentation, and BFS multi-hop queueing, it abstracts physical-layer SINR capture, 802.11a/g PLCP preambles, RTS/CTS hidden-terminal mitigation, and IPv4/UDP routing. Phase 5 (`omnetpp_inet_layer/`) provides the C++ `TraceEventPlayer` architecture to replay the exact same JSON trace contracts over INET `Ieee80211ScalarRadioMedium` when native Windows OMNeT++/INET runtimes are installed.
