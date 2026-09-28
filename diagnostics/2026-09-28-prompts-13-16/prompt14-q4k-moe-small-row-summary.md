# Prompt 14 — Q4_K small-row MoE dispatch A/B

## Decision

**PASS: candidate evaluated and rejected.** Median ordinary decode changed from **36.027 to 35.773 tok/s (-0.705%)**. Both 2-row and 3-row dependency-bound completion times were slightly worse. This is no demonstrated gain, not a claim of a statistically established regression. Original grouped dispatch retained; no second candidate tested.

- Starting/final HEAD: `b58deeb81e19b250222fe259d223339c8634543a`. Branch `master`; **no commit**.
- Local rollback branch: `backup/pre-prompt14-q4k-moe-small-row` at that HEAD.
- GitHub never contacted; no push, remote branch, tag, release, or settings change. Local refs/config unchanged after the requested rollback branch; `origin/main` absent locally before/after. No remote-state query was made.
- All **123 original file hashes**, Git status/index/refs/config, prepared executor, and prepared MMVQ source verified restored. The qualified native executable is back in `dist/engine`. All original unrelated changes preserved.

## Exact baseline and candidate

Baseline call chain: `Executor::GatedExperts -> qfn_mmq_moe_gated_vec -> moe_vector_projection -> mul_mat_vec_moe_gated -> launch_moe_grouped<Q4_K>`. Batch activation quantization remains unchanged. Gate/up are fused with SwiGLU, using separate 32-lane waves for their original sums.

Effective production dimensions: K=2560, output channels=640, 10 selected experts per token; verification token rows=2/3. There are 47 Q4_K layers per round (46 in the suffix); the other layer uses Q5_K and is unchanged.

| Dispatch | 2-row | 3-row |
|---|---|---|
| Baseline grouping prep | 1 grid, block 128 | 1 grid, block 128 |
| Baseline grouped gate/up | 1 launch, grid(320,20), block 64 | 2 launches, grid(320,30), block 64; multi/single-request variants |
| Candidate gate/up | 2 existing scalar launches, grid(320,10), block(32,2) | 3 existing scalar launches, same grid/block |
| Quantization + gate/up/prep nodes per Q4 layer | 3 in either variant | 4 in either variant |

**Existing small-row path reused: YES.** The candidate calls `mul_mat_vec_q_moe<GGML_TYPE_Q4_K,2,true>` once per token, with correct input, routed-ID, and output offsets. It skips grouped slot preparation only for the selected Q4_K operation. No kernel body, reduction tree, weight interpretation, activation, or output type changed. It trades grouped cross-token weight reuse for smaller per-token grids; it does not reduce total launch count.

Gate: `FNV_Q4_SMALL_ROW=1`, speculative verification, Q4_K, rows exactly 2 or 3. Rows 1 and 4+ use original dispatch. Down projection, router, shared expert, HC, QSA, DeltaNet, vocabulary/head, MTP, PLE and lookup were untouched.

Trial files (all restored/removed):

- `CMakeLists.txt`
- `src/native/main.cpp`
- `src/native/moe_small_row_json.hpp`
- `gufo-overlay/src/models/qwen38_flash_next/kernels/rocm/executor.cpp`
- `gufo-overlay/src/models/qwen38_flash_next/kernels/rocm/mmq/mmvq.hip.cpp`
- `gufo-overlay/src/models/qwen38_flash_next/kernels/rocm/mmq/small_row_moe.hpp`
- `tests/q4_small_row_test.hip.cpp`

The new MMVQ overlay copied the exact effective pinned file before adding the dispatch block; original pinned source hash is preserved with `original-mmvq.hip.cpp`. Trial copies/patch and executable remain under `benchmarks/prompt14/` only. The normal build script clears the disposable trial build cache on the next build.

## Focused correctness and graph check

Built using **scripts/build.ps1 -SkipBundledTests**, same installed compiler/runtime/gfx1151, successful first build. The focused test is a narrowed adaptation of the pinned `routed_wmma_ops_test.cpp::CheckVectorGrouping`: Q4_K rows 1/2/3, output channels 640/641, ordinary and unsaturated inputs, repeated/disjoint expert routes, duplicate slots, inactive IDs, nonfinite scales and output guards. It compares bitwise with separate scalar gate/up and GPU SwiGLU. Both variants passed. Three graph replays per tested shape were byte-identical too. No tolerance relaxed; no full suite; no external numeric thresholds used.

## Benchmark protocol

Same diagnostic executable for A/B; process environment selects dispatch before capture. Each configuration keeps its model/MTP loaded once. **A: 3 requests; B: 4 requests**. First requests remain in medians—no excluded warmup. A was stable; B's first three were within 1.5% of A, triggering the specified fourth. Gain was negative, so no 1–2% confirmation pair or secondary workloads were triggered.

Canonical fixture: 1165 prompt / 511 completion, seed 12345, thinking OFF, original sampler, Q8_0 + Latin + halo_greedy/max 6, sticky 6→16, context 131117, tuning `none,+flag_waits`, PLE cached-overlapped-random/128 workers/8 MiB, 5 GiB guard.

An initial baseline attempt stopped after two correct requests when the third returned HTTP 400 near the memory floor. Its error body was not captured, so the exact rejection text is unknown. Host availability was subsequently observed below 5 GiB even with the engine stopped. After memory recovered to 7.91 GiB, the entire A sequence restarted. The incomplete attempt is retained under `incomplete-A/` and excluded from the complete A/B comparison. The guard was never weakened. The completed B process stayed above 6.69 GiB after requests.

Raw decode speeds:

- A: 36.0841,36.0270,36.0263 tok/s.
- B: 35.9149,35.8123,35.7146,35.7335 tok/s.

| Median metric | A baseline | B candidate |
|---|---:|---:|
| Decode tok/s | 36.027 | 35.773 |
| Prefill tok/s | 1327.545 | 1333.430 |
| TTFT ms | 957.680 | 954.326 |
| Proposal ms | 1376.467 | 1378.588 |
| Dependency-bound verification ms | 12359.636 | 12455.722 |
| MTP-only cycle ms | 14099.025 | 14197.948 |
| All speculative cycle ms | 14171.829 | 14272.146 |
| Rollback ms | 286.982 | 286.927 |
| 2-row completion ms/round | 44.92453 | 45.11917 |
| 3-row completion ms/round | 50.44437 | 50.87355 |
| Existing sync API wait ms | 1572.523 | 1577.419 |
| WDDM GiB | 86.628429 | 86.628429 |

Every request has **132 two-row rounds and 120 three-row rounds**. Candidate latency increased 0.43% / 0.85% respectively. Completion-wall timing spans prefix launch through the existing required completion, including host dispatch and PLE dependency; it is not isolated suffix or pure GPU time. Unreliable existing HIP-event fields in raw request telemetry were not used for the decision.

## Execution proof and preservation

| Logical Q4_K gate/up calls/request | A | B |
|---|---:|---:|
| Baseline grouped | 12126 | 282 |
| Candidate | 0 | 11844 |
| Candidate rows2 | 0 | 6204 |
| Candidate rows3 | 0 | 5640 |
| Other-row fallback (subset of baseline) | 282 | 282 |

Counters originate at actual dispatch callbacks. Capture construction is excluded from executed-call totals; its recorded dispatch delta is applied once at each graph launch. Zero missing templates/overflow. These are host-attributed logical call counts, not device instruction counters. This avoids falsely counting only host graph construction.

- Verification graph replays: A 506/516/516; B 506/516/516/516. Verification captures 6 on each process's first request, 10 subsequently. Existing total profiled captures A 12/12/10, B 12/12/10/10. Graph replay policy unchanged.
- All output SHA-256 values: `9b26a2ddd9518368939d4d29f107f398e793acd5f5cbdb926b2a9ffaa900c4f7`.
- MTP: 391 drafted, 253 accepted, **64.705882% acceptance** in both. MTP-only accepted/output = 253/510 = **0.496078**. Lookup: one round, 6 drafted, 0 accepted. No semantic changes.
- Synchronization: **361 stream syncs, 0 event syncs, 0 device syncs** per request in both; existing flag waits preserved. Sync API wait changed only about 4.90 ms/request.
- WDDM **86.628429 GiB** after every complete request; no persistent candidate allocations, no per-request device-memory growth observed. 5 GiB host guard unchanged. This bounded run is not a long-duration leak test.
- Code-edit sanity: **not run**, primary candidate did not win. New-code sanity: **not run**, same reason.

## Next target

Future task only: Q8 activation-quantization/dispatch overhead in the same 2/3-row verification suffix (474 quantize_q8_1 nodes per suffix in Prompt13.2). This is another structural candidate, not a proven GPU bottleneck. Use a separate narrow A/B with exact output and trusted completion-wall metrics. No additional kernel candidate was tested in Prompt14.

## Local evidence

`prompt14-q4k-moe-small-row-ab.json` contains per-run metrics, decisions, exact path descriptions, test results and restoration checks. `prompt14/A.json` and `B.json` retain requests, complete telemetry and health; `math-test.log`, `production-build.log`, `trial-source/`, `trial.patch`, `trial-engine.exe`, and `restoration.json` preserve reproducibility. No artifacts uploaded.

PROMPT 14 STATUS: PASS
SMALL-ROW Q4_K MOE DISPATCH RETAINED: NO
PRODUCTION CONFIG CHANGED: NO
GITHUB/REMOTE UPDATED: NO
