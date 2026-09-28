# Prompt 13.2 — Production verification graph topology and timing

## Decision

**PASS, Outcome C.** Negative GPU timestamps remain unresolved. The complete 2/3-row graph structure, application dependencies, and source attribution are now mapped. Use **DEPENDENCY-BOUND GRAPH COMPLETION WALL TIME**; do not average invalid events or infer kernel cost from node counts.

- Starting and final HEAD: `b58deeb81e19b250222fe259d223339c8634543a` (`master`). **No commit.** Local rollback branch `backup/pre-prompt13-2-verify-graph-timestamps` points to that HEAD.
- GitHub never contacted; no push, remote branch, tag or release modification. Local refs/config unchanged after the requested rollback branch. `origin/main` is absent locally before/after, so no remote-state readback was claimed. Frozen backup unchanged by this task.
- All **123 original source/configuration/runtime file hashes** match after restoration. Original dirty/untracked work, index, Git status, and prepared Gufo executor preserved. Trial source, patch, executable, build logs, and raw measurements retained only under `benchmarks/prompt13_2/`. The disposable build directory contains the trial build; use restored `dist` for production or the normal build script to rebuild.
- Runtime verified again: HIP **7.15.26333**, TheRock **10.0.0rc4**, gfx1151 Radeon 8060S; production `amdhip64_7.dll` SHA-256 `546fb3d6e2d2194a9526fb94ec2fd3aa5b92a48a7595f04efece80162047ef69`.

## Exact topology

Indices below are zero-based. For every graph, node i depends only on i-1, node 0 is the sole root, and the last node the sole leaf. Four shapes dumped once during warmup; capture APIs and kernel symbol lookups all succeeded.

| Graph | Nodes | Kernels | Copies | Edges |
|---|---:|---:|---:|---:|
| 2-row prefix | 39 | 37 | 2 | 38 |
| 2-row suffix | 1690 | 1685 | 5 | 1689 |
| 3-row prefix | 40 | 38 | 2 | 39 |
| 3-row suffix | 1740 | 1734 | 6 | 1739 |

All four have **zero child, event record, event wait, host, empty, semaphore or allocation nodes**. Therefore internal/external event-object aliasing or updating an internal event node cannot explain these samples.

`Executor::Forward` selects a session-cached `hipGraphExec_t` by rows, logits rows, verify/sparse/download/compact/profile bits. Prefix adds bit35; profile bit36 is the existing profiling policy; compact64 uses bit37. Observed prefix keys are 244813266946 / 244813332483 and suffix keys 210453528578 / 210453594115 (2/3 rows). The captured `hipGraph_t` is instantiated then destroyed; its executable is cached in `session.graphs_`. `Session::Reset -> TrimRollback(0)` frees verification graphs referencing released rollback rows; observed graph generations are 1, 2, 3. Each failed sample records the actual executable, key, generation, and event-pair reuse count.

All observed launches/events use stream **2140038175936** (`0x1f244371cc0`), created by `hipStreamCreate`, flags **0**, with thread-local capture. The BLAS handle is assigned this stream. No application cross-stream/event dependency was observed. Runtime-internal queues are not exposed by graph introspection and are not ruled out.

### Dependency chain and natural wait

Host starts PLE read -> same-stream prefix (uploads/embedding + layer0) -> existing host PLE-read completion dependency -> same-stream suffix (PLE injection + layers1–47 + head/compact outputs) -> external stop event -> existing `SignalDone` kernel -> existing nonblocking `hipStreamQuery` submission hint -> coherent host-flag polling -> CPU candidate/certificate/acceptance reads.

The flag wait is ordered after the complete suffix and its D2H copies. Under the documented same-stream graph semantics it covers the verification work needed by the CPU. Both timing-event queries report completion before elapsed reads. Existing MTP-chain D2H readiness and rollback-frontier stream synchronizations remain outside this graph. Counts unchanged: 257 + 104 = **361 stream syncs/request**, zero event/device syncs; no added wait.

### Copy nodes

| Piece | 2-row node indices / bytes | 3-row node indices / bytes |
|---|---|---|
| Suffix PLE staging H2D | 0 / 20480 | 0 / 30720 |
| Target hidden retain D2D | 1671 / 81920 | 1717 / 122880 |
| Compact candidate D2H | 1684,1688 / 1280 each | 1730,1734,1738 / 1280 each |
| Frontier retain D2D | 1689 / 993280 | 1739 / 993280 |

Prefix also has control/token H2D nodes (full parameters saved in JSON). Candidate buffer reuse is sequential: selector then D2H for each row on the same chain. No asynchronous child stream escape is visible.

## Working synthetic versus production

| Property | Prompt13.1 synthetic | Production 2-row | Production 3-row |
|---|---|---|---|
| Explicit launch stream | nonblocking | flags0 | flags0 |
| Prefix + suffix nodes | one kernel | 39 + 1690 | 40 + 1740 |
| Total kernels / copies | 1 / 0 | 1722 / 7 | 1772 / 8 |
| Child/event/host nodes | 0 | 0 | 0 |
| Application multistream dependencies | none | none observed | none observed |
| Completion mechanism | final stop-event synchronize | existing SignalDone flag wait | existing SignalDone flag wait |
| Bracket | external same-stream | external same-stream per executable | external same-stream per executable |
| Graph/state lifetime | fixed deterministic scratch | recurrent/KV/rollback/output state, exec recaptured per request | same |

Synthetic 44 standard-event trials remain the known-good control (not rerun): graph batches 1/8/32/128 approximately 0.463/3.575/14.167/56.706 ms. Production's smallest **observed correlation** is short prefix versus long mixed-copy suffix, which adds D2H. Neither copy direction, graph size, stream flags, nor completion mechanism was independently varied; **no cause is proven**.

## Controlled experiments and ordering

**A: rejected.** Every record, launch, query, and elapsed call returned `hipSuccess (0)`; no NotReady, invalid handle, zero duration, pending overwrite, or ordering failure. Yet **233/504 measured suffix samples (46.23%) are negative**. The remaining positive values are not automatically trustworthy: many are microseconds while dependent completion is tens of milliseconds.

| Measured shape | Negative / samples | Elapsed median request 1 / request 2 (ms) |
|---|---:|---:|
| 2-row prefix | 0 / 264 | 1.056935 / 0.978695 |
| 2-row suffix | 110 / 264 | 0.022995 / 0.006105 |
| 3-row prefix | 0 / 240 | 1.249125 / 1.151205 |
| 3-row suffix | 123 / 240 | -0.000985 / 0.000825 |

Representative failed ordering, measured1:

- 2-row: start **3704**, launch **3705**, stop **3706**, natural wait begin/end **3707/3708**, elapsed read **3710**; -0.111439 ms; graph generation 2, pair reuse 270; both queries 0.
- 3-row: start **3466**, launch **3467**, stop **3468**, natural wait begin/end **3469/3470**, elapsed read **3472**; -0.087679 ms; graph generation 2, pair reuse 253; both queries 0.

Failures recur across graph generations 1–3 and broad reuse ranges, not a single stale graph handle. Completion status proves API completion, not raw timestamp validity. No supported public raw event-timestamp accessor was found; no DLL patching, undocumented event internals, or driver hooks used.

**B: positive inclusive intervals, not recovered GPU graph timing.** Naturally consecutive same-row verification rounds still interleave prefix, proposal/MTP, rollback, host work, and required waits. Warmup's 3-row batch reached8 suffix launches (583.325 ms). Measured 3-row batches ended at 2 on shape change: 241.486 / 187.775 ms. Measured 2-row batches also ended at 2: 121.010 / 106.429 ms. All API statuses 0. Shape-break brackets include the next prefix; the QPC consumption endpoint can additionally include the next suffix. These values are not isolated suffix GPU totals and must not be divided into GPU time/launch. No extra work or synchronization was generated. Strict graph-only consecutive batches are unavailable in this path.

**C: conditional skip.** Graph-clone/event-node APIs exist, but the clone would retain live recurrent/KV/rollback/scratch and host-output pointers. There is no safe isolated-state replay harness. Launching the clone would change production state; making one would require substantial state copying/rebinding beyond this bounded task. No live graph substitution or graph-event mutation occurred.

## Proven source mapping

Capture-frontier markers (`hipStreamGetCaptureInfo_v2`) and each node's kernel symbol establish the mapping; no per-node timers were added. Every node is covered by source ranges in the JSON. `hipKernelNameRefByPtr` was declared but unexported; exported `hipGetFuncBySymbol -> hipKernelNameRef` succeeded for every kernel. Demangled symbols, grid/block dimensions, exact dependencies, source definition paths/lines/hashes are saved.

| Stage / source function | 2-row suffix indices | 3-row suffix indices |
|---|---|---|
| `Executor::Ple`, layer1 | 0–12 | 0–12 |
| layer1 `HcMix` attention | 13–19 | 13–19 |
| layer1 `LinearAttention` | 20–28 | 20–28 |
| layer1 `Combine` attention | 29 | 29 |
| layer1 `HcMix` FFN + `Moe` | 30–47 | 30–48 |
| Remaining trunk through layer47 | 48–1670 | 49–1716 |
| Hidden retain copy | 1671 | 1717 |
| `HcMix` output head | 1672–1678 | 1718–1724 |
| `Dense` vocabulary projection | 1679–1680 | 1725–1726 |
| Compact64 selectors + per-row D2H | 1681–1688 | 1727–1738 |
| Frontier D2D | 1689 | 1739 |

Key repeated suffix families: 474 `quantize_q8_1`, 239 `qfn_q8_shallow_kernel`, 95 `qfn_q8_hc_down_kernel`, and **47 versus93 `mul_mat_vec_moe_grouped`**. The latter difference is proven by `mmq/mmvq.hip.cpp::launch_moe_grouped`: for Q4_K with tokens >= 3 it launches distinct multi-request and single-request variants. The predicates partition expert cases; this is not redundant work. Counts describe structure, not runtime cost.

## Trusted wall attribution, correctness, and resources

| Shape | Rounds/request | Completion-wall ms/round | Total ms/request | Decode share |
|---|---:|---:|---:|---:|
| 2-row | 132 | 47.76165 | 6304.538 | 42.151% |
| 3-row | 120 | 54.33719 | 6520.462 | 43.595% |

Combined **85.746%** of decode. This is one QPC interval per whole verification round, from before prefix through the existing required completion, including host launch and PLE dependency. It is additive between rounds, **not pure GPU time or isolated suffix time**. Prior 13.1 reference remains45.00/51.02 ms and86.06%; current diagnostic measured47.76/54.34 ms. No negative GPU data was averaged into it.

Exactly one warmup plus two measured requests. All have 1165 prompt / 511 completion tokens, seed 12345; 391 MTP drafts/ 253 accepted, lookup 6 drafts/0 accepted, one lookup round. SHA-256 unchanged: `9b26a2ddd9518368939d4d29f107f398e793acd5f5cbdb926b2a9ffaa900c4f7`. Model and MTP loaded once; configuration and algorithms unchanged.

Measured throughput 32.721 / 35.741 tok/s, median **34.231**. Versus historical 13.1 median 36.460, delta -6.11%; this is **not a causal overhead estimate**. No contemporaneous OFF pair was allowed by the request cap; under-2% overhead is not established. No diagnostic remains in the production source/executable.

WDDM **86.628429 GiB after every request**, 20 diagnostic events fixed over the process lifetime, peak 2 individual spans pending, zero overflow/pending at request ends. No growth observed over three requests; not a long-run leak proof. The 5 GiB host guard stayed enabled. Task-started engine stopped before restoring its executable.

Build script only, focused requests only, no full suite. Two diagnosed build failures (JSON integer casts, unavailable name-helper export), then successful native production-script build. Cached dependencies used with network protocols/proxies blocked. No external profiler setup or driver/runtime changes.

## Exact Prompt 14 scope

Prompt 14: one gated, correctness-preserving A/B confined to Executor::GatedExperts -> qfn_mmq_moe_gated_vec -> launch_moe_grouped -> mul_mat_vec_moe_grouped for 2/3-row Q4_K verification. Inspect the split multi-request/single-request dispatch (false/true variants) at tokens >= 3. The suffix has 46 Q4_K layers with the extra dispatch (47 including prefix); this is a structural candidate, not a measured GPU bottleneck. Preserve expert masks, slot ownership, quantization, reduction order and arithmetic. Judge one candidate by dependency-bound full verification-round wall time and end-to-end decode against an OFF control, with exact hashes/speculation counts. Do not remove either pass merely because it is an extra launch: they process disjoint active-expert cases. No MTP, sampling, PLE, shortlist, D2H, wait, or graph-boundary changes.

## Evidence

- `benchmarks/prompt13_2-verification-graph-topology.json`: full topology/source mapping, all negative sample classifications, comparisons, decisions, and checks.
- `benchmarks/prompt13_2/warmup.json`, `measured1.json`, `measured2.json`, `runs.json`: raw API/sequence/timing/health data.
- `benchmarks/prompt13_2/production-trial-source/`, `production-trial.patch`: reproducible gated diagnostic trial; OFF unless environment gate and existing request profiler enabled.
- `benchmarks/prompt13_2/restoration.json`: 123-file and Git preservation verification.

PROMPT 13.2 STATUS: PASS
NEGATIVE TIMESTAMP CAUSE IDENTIFIED: NO
TRUSTWORTHY PRODUCTION GPU TIMING AVAILABLE: NO
2/3-ROW VERIFICATION PATH MAPPED: YES
PRODUCTION CONFIG CHANGED: NO
GITHUB/REMOTE UPDATED: NO
