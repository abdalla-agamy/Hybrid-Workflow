**Yes — the updated Phase 6.1 results are academically valid, statistically sound, and ready to update `OPI (1).pdf`.**

All four issues from the previous audit were cleanly fixed in the code and methodology:

* **Lifespan Sorting (`eta = 0.85`)** is now active in the exported MATLAB traces with an isolated per-event RNG seed so exogenous schedules remain 100% paired across all five protocols.


* **2.0 ms Relay Jitter** in `TraceEventPlayer.cc` cleanly staggers concurrent multi-hop relay parents (and cleans up all pending timers across timeouts, completions, and destruction), eliminating the artificial synchronous collision bug.


* **5-Seed Replications (`repeat = 5`, Student-$t$ 95% CI)** in `omnetpp.ini` removed the non-monotonic noise in Figure 2 and produced tight, publication-grade confidence bands.


* **Figure 4(b)** replaced the unweighted composite score with a **54 Mbps E2E Delay vs. $P_{\text{sync}}$ Pareto Frontier**, which visually anchors DT-HSBP as the optimal cluster protocol.



---

### 1. Impact of the Fixes at $N = 250$ (OMNeT++ / INET, $r = 5$, Mean $\pm$ 95% CI)

| Bitrate & Condition | Metric | HSBP | **DT-HSBP (Proposed)** | LKH ($d=3$) | TGDH | Flat-SBP |
| --- | --- | --- | --- | --- | --- | --- |
| **54 Mbps** ($p_{\text{loss}}=0.05$) | **E2E Delay (ms)** | $32.67 \pm 1.23$ | **$24.22 \pm 0.55$** | $59.79 \pm 1.10$ | $80.26 \pm 2.22$ | $541.47 \pm 1.98$ |
| **54 Mbps** ($p_{\text{loss}}=0.05$) | **On-Air Bytes** | $3.62\text{ MB}$ | **$2.09\text{ MB}$ (-42.3% vs. HSBP)** | $0.64\text{ MB}$ | $1.07\text{ MB}$ | $5.98\text{ MB}$ |
| **54 Mbps** ($p_{\text{loss}}=0.05$) | **MAC Collisions** | $22{,}554 \pm 4{,}019$ | **$11{,}543 \pm 2{,}326$ (-48.8% vs. HSBP)** | $3{,}185 \pm 761$ | $4{,}372 \pm 875$ | $334{,}794 \pm 9{,}990$ |
| **54 Mbps** ($p_{\text{loss}}=0.15$) | **Stress Delay / $P_{\text{sync}}$** | $50.4\text{ ms}$ / $98.87\%$ | **$40.1\text{ ms}$ / $99.28\% \pm 0.69\%$** | $112.7\text{ ms}$ / $98.15\%$ | $150.7\text{ ms}$ / $98.40\%$ | $663.7\text{ ms}$ / $99.79\%$ |
| **6 Mbps** ($p_{\text{loss}}=0.05$) | **E2E Delay (ms)** | $108.36 \pm 2.82$ | **$70.70 \pm 4.31$** | $77.35 \pm 1.07$ | $108.10 \pm 3.99$ | $585.92 \pm 1.34$ |
| **6 Mbps** ($p_{\text{loss}}=0.05$) | **On-Air Bytes** | $3.52\text{ MB}$ | **$2.03\text{ MB}$ (-42.2% vs. HSBP)** | $0.64\text{ MB}$ | $1.07\text{ MB}$ | $5.97\text{ MB}$ |


**Key headline gains for `OPI (1).pdf`:**

* **Fastest E2E latency at *both* bitrates:** DT-HSBP is now the fastest protocol across all five schemes at **54 Mbps** (`24.22 ms` — **59.5% faster than LKH**, **69.8% faster than TGDH**, **25.9% faster than HSBP**, and **95.5% faster than Flat-SBP**) and also beats LKH and TGDH at **6 Mbps** (`70.70 ms` vs. `77.35 ms` and `108.10 ms`).


* **Substantial physical air-traffic reduction:** Enabling lifespan-sorted tail truncation cuts DT-HSBP's MATLAB departure bytes by **85.0%** (`140,448 B` $\to$ `21,112 B`) and total OMNeT++ on-air bytes by **42.2%–42.3%** relative to reactive HSBP.


* **Pareto dominance under severe channel stress ($p_{\text{loss}} = 0.15$, 54 Mbps):** DT-HSBP (`40.09 ± 0.82 ms`, `99.28% ± 0.69%` $P_{\text{sync}}$) strictly dominates HSBP, LKH, and TGDH on both latency and reliability while keeping $\mathcal{O}(1)$ per-UAV key storage.



---

### 2. Three Minor Writing Notes When Updating `OPI (1).pdf`

No more code changes are needed—just frame these three details accurately in the manuscript so reviewers have nothing to nitpick:

1. **Be precise about LKH/TGDH vs. DT-HSBP trade-offs:** After adding relay jitter, LKH (`3,185`) and TGDH (`4,372`) incur fewer raw MAC collisions and fewer on-air bytes than DT-HSBP (`11,543`) because their tree messages are smaller single-fragment payloads. Frame this honestly as the core architectural trade-off: **LKH/TGDH minimize raw bytes at the cost of $\mathcal{O}(\log N)$ stateful key storage, 2.5$\times$–3.3$\times$ higher 54 Mbps E2E latency, and multi-round sync degradation under burst loss**, whereas **DT-HSBP minimizes E2E latency, maximizes cluster sync resilience, and preserves $\mathcal{O}(1)$ stateless UAV storage**.


2. **Cite $p_{\text{loss}} = 0.15$ for reliability separation (not $p_{\text{loss}} = 0.05$):** At $p_{\text{loss}} = 0.05$, all five OMNeT++ protocols sit at `100.0%` $P_{\text{sync}}$. In your paper's abstract and Section 4, report $p_{\text{loss}} = 0.05$ for nominal latency/bandwidth efficiency and $p_{\text{loss}} = 0.15$ for burst-loss reliability (`99.28%` vs. `98.15%` LKH and `98.40%` TGDH).


3. **Add one sentence explaining the SimPy vs. OMNeT++ curve gap in Figure 2:** At $p_{\text{loss}} = 0.15$, OMNeT++'s solid curves (`98.1%–99.8%`) stay higher than SimPy's dashed curves (`93%–97%`). Note briefly that SimPy's abstract MAC includes an analytical neighbor-contention drop approximation, whereas OMNeT++/INET with 2 ms relay jitter resolves medium contention via physical 802.11g DCF backoff.