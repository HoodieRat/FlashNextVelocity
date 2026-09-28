# Prompt 13 — BLOCKED; candidate rejected and rolled back

**Reason:** same-stream HIP events did not yield trustworthy verification graph timing. Two ON requests failed 139 and 158 of 908 spans; some accepted suffix intervals were implausibly short. GPU totals and internal costs remain unresolved. No optimization is justified from those values.

- Starting and final HEAD: `b58deeb81e19b250222fe259d223339c8634543a`.
- `NO LOCAL COMMIT - OVERLAPS PRESERVED WORK`. Local rollback branch exists.
- GitHub/remotes: no contact or changes. Exactly one production build passed; no full suite.
- Exact pre-task source, production EXE, configuration, UI and unrelated work restored; all 123 preserved hashes, Git status and index match.
- Rejected candidate source, binary and an apply-checked patch are under `benchmarks/prompt13/rejected-candidate` and `instrumentation.patch`. They are diagnostic evidence, not qualified runtime artifacts.

## Request and overhead

| Run | Prompt/output | Decode tok/s | Prefill tok/s | TTFT ms | Captures | Replays | Timer errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| control1 | 1165/511 | 34.130 | 770.3 | 1587.2 | 12 | 890 | 0 |
| control2 | 1165/511 | 34.561 | 1243.3 | 1011.4 | 12 | 908 | 0 |
| profile1 | 1165/511 | 35.306 | 1280.1 | 996.1 | 10 | 908 | 139 |
| profile2 | 1165/511 | 34.728 | 1245.0 | 1013.1 | 10 | 908 | 158 |

Control median **34.345 tok/s**; ON median **35.017 tok/s**; observed delta **+1.95%** (apparent slowdown -1.95%). This does not establish zero overhead or a performance gain. Controls include cold/cache warmup and run below the historical ~36.1 tok/s.
Median TTFT: 1299.3 → 1004.6 ms (-294.7 ms). Warm control TTFT is 1011.4 ms. HIP synchronization count delta: **0**. Warm replay count delta: **0**; including cold control, median replay count changes 899 → 908.
The two controls differed by 1.26%, so no third control was needed. A third ON request was unnecessary after the timing validity failure repeated; more invalid samples cannot establish reliability.

## Speculation, transfers, and determinism

- Every request: 258 speculative rounds, 257 MTP rounds; 391 MTP drafts, 253 accepted (**64.706%**); MTP accepted/output **0.49511**. One lookup round proposed 6 and accepted 0; no promotion.
- All four output SHA-256 values: `9b26a2ddd9518368939d4d29f107f398e793acd5f5cbdb926b2a9ffaa900c4f7`.
- Model, MTP, sampler, lookup settings and one loaded runtime remained unchanged.
- Verification: 655 output/compact rows, 104 full rows, 41,920 candidates, **104,139,520 bytes**. Bytes reconcile as `104 × 248320 × 4 + 655 × (256 × 4 + 64 × 4)`. The compact row includes the fixed ID prefix plus 64 float logits.
- Certificates: 357 successes, 0 fallbacks, 0 unsupported-sampler fallbacks; 104 rejection-frontier downloads.
- CPU accept/residual median: **9.996 ms**. Exact D2H GPU duration remains **unresolved**; even existing external-transfer event measurements failed validation.

## Graphs and internal stages

- ON: 908 replays = 516 verification pieces + 392 MTP graphs. Verification has 258 prefix/suffix pairs. No eager verification executions occurred in the warm control or ON requests; cold control had 10 eager verification pieces (5 pairs).
- Warm target graphs are recaptured after the existing rollback-row trimming discards captured pointers. ON captures: 10 target pieces per request. Control 2 additionally first-captured two previously warmed MTP shapes; no diagnostic graph-key bit or topology change was introduced.
- Total replay GPU time, verification GPU time and GPU ms/round: **unresolved/invalid**. No accepted partial timing is promoted to an exact total.
- Trunk, HC/head, vocabulary projection and compact selection: **unresolved inside replay**. No eager composition experiment was run.
- **QSA EXECUTED BUT INTERNAL REPLAY TIMING UNRESOLVED.** Indexer key projection/store executes during 258 target forwards. Sparse selector/attention: not executed for this shallow workload.

| Verification rows / draft depth | Prefix replays | Suffix replays | GPU time |
|---|---:|---:|---|
| 2 / 1 | 132 | 132 | unresolved |
| 3 / 2 | 120 | 120 | unresolved |
| 4 / 3 | 1 | 1 | unresolved |
| 5 / 4 | 4 | 4 | unresolved |
| 7 / 6 | 1 | 1 | unresolved |

## Host waits and accounting

These are ON medians. GPU execution overlaps host completion waits; waiting is not automatically removable overhead.

| Reason | Count/request | Host ms |
|---|---:|---:|
| MTP chain D2H readiness (hipStreamSynchronize) | 257 | 1226.344 |
| rollback frontier dependency (hipStreamSynchronize) | 104 | 163.213 |
| verification/plain forward completion (coherent flag wait) | 258 | 11741.147 |
| MTP forward completion (coherent flag wait) | 1 | 1.449 |

HIP stream synchronizations: **361**, device/event synchronizations: **0**. HIP wait: **1389.557 ms**. Coherent flag waits: **11742.596 ms**; the latter explain much of the old generic verification bucket.
Raw proposal/catch-up: **1304.355 ms**; verification: **13078.062 ms**; rollback: **184.189 ms**; PLE wait: **175.242 ms**. These raw values overlap the wait rows.

| Exclusive host stage | Median ms | Decode % |
|---|---:|---:|
| Plain forward host | 0.000 | 0.000% |
| Spec proposal host | 78.210 | 0.536% |
| Spec verification host | 1173.922 | 8.047% |
| PLE wait | 175.242 | 1.202% |
| Rollback | 20.976 | 0.144% |
| Sampling | 4.165 | 0.029% |
| Synchronization | 13132.153 | 89.979% |
| Other | 9.345 | 0.064% |

Exclusive rows reconcile to **14594.012 ms** median decode wall time; per-request residual <0.001 ms. The residual Other row is explicit, and no independent GPU/QSA time is added. This is a host timeline, not a kernel cost breakdown.

## Memory and focused checks

- WDDM after all four requests: **86.628429 GiB**, unchanged. Five-GiB host guard preserved.
- New events: **64 lifetime**, reused in both ON requests; pending peak 4, pending at completion 0, no slot overflow. RSS ends below its observed peak; four requests do not prove long-run leak freedom.
- PASS: gate OFF event behavior, bounded reuse, output/count determinism, graph/capture aggregation, synchronization aggregation, transfer arithmetic, exclusive accounting, runtime load identity, WDDM and exact rollback preservation.
- FAIL: replay GPU timer reliability and existing external D2H GPU timer reliability. These failures block retention and Prompt 13 acceptance.

## Ranked next investigations (host evidence; not optimization claims)

1. **target verification / ROCm graph profiling** — 13078.1 ms, 89.61% raw host share. 258 target verification rounds, 516 replayed pieces; coherent completion wait dominates. Internal kernel composition and GPU total remain unresolved. Measurement repair first. No basis to select HC, output projection, compact select, or QSA for optimization.
2. **MTP proposal/catch-up and chain completion** — 1304.4 ms, 8.94% raw host share. 257 chain-end HIP waits; same 391 drafts and 253 accepted in every request. Potential later proposal/chain study only after reliable GPU timing; required GPU work is included in waits.
3. **PLE host dependency** — 175.2 ms, 1.20% raw host share. Median 175 ms of host waits; prefix GPU work can overlap disk reads. Total worker read-ms is not decode wall time. Small measured wait ceiling; lower priority than verification. No PLE change justified in this task.

**Prompt 14:** First isolate HIP/WDDM event correctness in a minimal same-stream graph replay diagnostic: log query status, elapsed API status, raw signed elapsed values and both event completions; compare with an independent supported GPU timing source. Preserve production graph topology and fixed-seed output. Do not optimize a kernel until replay timing is validated.

The containing verification/completion bottleneck is identified. Its internal kernel bottleneck is not. The requested GPU measurement prerequisite must be repaired before choosing a kernel optimization.

PROMPT 13 STATUS: BLOCKED
PRODUCTION CONFIG CHANGED: NO
GITHUB/REMOTE UPDATED: NO
NEXT BOTTLENECK IDENTIFIED: YES (containing verification path only)
