# DT-HSBP Version 2: Closed-Loop 54 Mbps IEEE 802.11e EDCA, Rician Fading & Native Digital-Twin Evaluation

Generated: 2026-11-26 14:19:21
Runs Aggregated: **170** unique OMNeT++ 6.5.0 / INET 4.7.0 simulation runs across **43** parameter cells (`repeat = 5`, two-sided 95% Student-t CI with $t_{0.975,4} = 2.776445$).

## 1. Closed-Loop Protocol & Physical Layer Architecture
- **IEEE 802.11e EDCA (`54 Mbps`, `qosStation = true`)**: Stock INET `Hcf` / `Edcaf` maps `REKEY_BCAST_FRAG` / `REKEY_UNICAST_ACK` / `KEY_RESYNC_PULL` to `UP = 6 (AC_VO)`, encrypted `5 Hz` `128 B` `MISSION_C2_BCAST` to `UP = 4 (AC_VI)`, and `1 Hz` `64 B` `HEARTBEAT_DT_REPORT` + `5 s` `CH_SUMMARY_TO_SL` to `UP = 0 (AC_BE)`.
- **Air-to-Air Physical Channel**: `RicianFading` over `FreeSpacePathLoss` ($\alpha = 2.0$, $f = 2.412\text{ GHz}$, $P_{tx} = 100\text{ mW}$, sensitivity $-71.5\text{ dBm}$) tested across $K \in \{10, 4, 0\}\text{ dB}$ with `Ieee80211NistErrorModel`.
- **Closed-Loop Native Digital Twin**: Each follower's `DTAgent` measures continuous `SwarmMobility` telemetry and transmits in-band `1 Hz` `AC_BE` reports to its CH `DTServer`, which runs a linear constant-velocity Kalman innovation tracker ($D_i(t)$) without oracle knowledge. Only **DT-HSBP** acts proactively on $D_i(t) > \theta_{leave} = 2.5$, using collateral lookahead batching and **Lifespan-Aware Chain Sorting** ($O(1)$ tail truncation).

## 2. Headline Results at N = 250 UAVs (54 Mbps EDCA)
- At moderate fading ($K = 4\text{ dB}$), **DT-HSBP** achieves **39.8%** proactive exclusion, reducing C2 security exposure window to **3.84 packets** (**83.8%** reduction vs. HSBP, **86.8%** reduction vs. Flat-SBP).
- On-air rekey traffic at $N = 250$ drops to **59.9 KB** (**63.7%** vs. HSBP, **77.5%** vs. Flat-SBP), sustaining **99.92%** C2 decryption success rate vs. **83.63%** under Flat-SBP.
- Under severe Rayleigh multipath fading ($K = 0\text{ dB}$), DT-HSBP maintains **99.87%** C2 decryption reliability (vs. **81.16%** for Flat-SBP) and **2.3 ms** mean key-desync outage.
- **MAC Queue Overflow Drops**: `0` across all 170 simulation runs.

### Table 1: N = 250, 54 Mbps EDCA, Moderate Fading (Rician K = 4 dB, Mean ± 95% CI)
| Protocol | DT Hit | Rekey Bytes (KB) | Rekey Latency (ms) | C2 Decrypt Rate | C2 Exposure (pkts) | Desync Outage (ms) | Resync Pulls |
|---|---:|---:|---:|---:|---:|---:|---:|
| Flat-SBP | 0.0% | 266.3 ± 73.3 KB | 475.0 ± 0.3 ms | 83.63% ± 5.76% | 28.98 ± 7.73 | 563.7 ± 126.2 ms | 2356 |
| HSBP | 0.0% | 165.0 ± 60.9 KB | 227.9 ± 71.0 ms | 99.85% ± 0.29% | 23.75 ± 8.45 | 63.1 ± 130.0 ms | 40 |
| DT-HSBP | 39.8% | 59.9 ± 7.9 KB | 184.1 ± 84.9 ms | 99.92% ± 0.02% | 3.84 ± 2.35 | 10.9 ± 19.4 ms | 33 |
| LKH | 0.0% | 116.9 ± 24.2 KB | 411.0 ± 28.6 ms | 99.94% ± 0.06% | 28.98 ± 7.73 | 121.1 ± 154.3 ms | 15 |
| TGDH | 0.0% | 94.0 ± 29.7 KB | 409.7 ± 56.6 ms | 99.81% ± 0.22% | 28.98 ± 7.73 | 132.0 ± 101.4 ms | 45 |

### Table 2: N = 250, 54 Mbps EDCA, Severe Rayleigh Fading (Rician K = 0 dB, Mean ± 95% CI)
| Protocol | DT Hit | Rekey Bytes (KB) | Rekey Latency (ms) | C2 Decrypt Rate | C2 Exposure (pkts) | Desync Outage (ms) | Resync Pulls |
|---|---:|---:|---:|---:|---:|---:|---:|
| Flat-SBP | 0.0% | 268.3 ± 68.0 KB | 475.0 ± 0.3 ms | 81.16% ± 5.16% | 28.98 ± 7.73 | 627.9 ± 135.0 ms | 2626 |
| HSBP | 0.0% | 175.1 ± 83.5 KB | 323.8 ± 131.7 ms | 99.83% ± 0.29% | 23.75 ± 8.45 | 45.9 ± 114.8 ms | 52 |
| DT-HSBP | 39.8% | 57.9 ± 7.9 KB | 255.1 ± 48.4 ms | 99.87% ± 0.04% | 3.84 ± 2.35 | 2.3 ± 1.7 ms | 58 |
| LKH | 0.0% | 123.6 ± 27.7 KB | 439.7 ± 30.5 ms | 99.71% ± 0.46% | 28.98 ± 7.73 | 172.9 ± 100.2 ms | 58 |
| TGDH | 0.0% | 130.4 ± 45.9 KB | 442.6 ± 27.1 ms | 99.66% ± 0.20% | 28.98 ± 7.73 | 327.4 ± 209.0 ms | 66 |

## 3. Visualizations
![Scalability EDCA](figures/v2_fig1_scalability_edca.png)

![Fading Resilience](figures/v2_fig2_fading_resilience.png)

![Security Exposure and Traffic](figures/v2_fig3_security_exposure_dt.png)

![EDCA vs DCF Ablation](figures/v2_fig4_edca_vs_dcf_ablation.png)
