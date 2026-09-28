# Prompt 15 — Q4_K activation quantization reuse

## Decision

**PASS: OPTIMIZATION EVALUATED AND REJECTED.** Genuine duplicate preparation was proven and eliminated in the trial. Median ordinary decode was **35.921 → 34.881 tok/s (−2.896%)**; neither 2-row nor 3-row completion improved. Original production source and executable restored. No local commit.

Starting and final HEAD: `b58deeb81e19b250222fe259d223339c8634543a` on `master`. Requested local rollback branch: `backup/pre-prompt15-activation-quant-reuse`. GitHub was never contacted. No remote branch/tag/release/settings update, push, or PR. Local refs/config/index/status match the snapshot after creating the rollback branch. `origin/main` and the qualified tag are absent locally before/after; no remote-state readback is claimed. The frozen backup was untouched by this task.

## Proven preparation path

`Executor::Moe -> GatedDense(shared up/gate) -> Quantize -> qfn_mmq_quantize_q8_1 -> quantize_row_q8_1_hip -> quantize_q8_1`

Later in the same Moe call:

`MoeExperts -> GatedExperts -> qfn_mmq_moe_gated_vec -> moe_vector_projection -> quantize_row_q8_1_hip`

Both consume the identical, unchanged FP32 `x`. Shared-down between them consumes a different tensor. The router consumes FP32 without this preparation.

Prepared format is contiguous row-major `block_q8_1`: 32 int8 values plus FP16 scale and sum, 36 bytes/block. K=2560 gives 80 blocks and 2,880 bytes/row. The GPU quantizer uses the original 32-wide maxabs/sum reductions, scale=maxabs/127, zero or `roundf(x/scale)`, and `half2(scale,sum(x))`. The implementation stores sum(x), despite a stale struct comment suggesting a reconstructed quantized sum. No arithmetic, scale, rounding, or format changed.

Each wrapper launches one separate GPU quantization kernel, no activation copies. Dense maps rows to grid Y; routed maps them to Z. Both flatten to the same input/output offsets, use the same kernel, and ignore the passed weight type. Focused tests confirmed bitwise equivalence.

| Scope per complete verification round | 2-row | 3-row |
|---|---:|---:|
| Eligible Q4 layers / unique batch tensors | 47 | 47 |
| Unique logical activation rows | 94 | 141 |
| Baseline compatible preparation launches | 94 | 94 |
| Minimum / candidate preparation launches | 47 | 47 |
| Redundant launches removed | 47 | 47 |
| Baseline prepared row instances | 188 | 282 |

One eligible layer is in the prefix, 46 in the suffix. Layer2 uses Q5 and is excluded. Gate/up and all selected experts already share the routed preparation; the redundancy is between shared-expert and routed-expert consumers, not between expert slots.

Saved graph examples: prefix shared preparation node28, routed node32; layer1 suffix shared node38, shared-down node40, routed node42. The graph audit records every eligible layer. Kernel arguments were not captured in Prompt13.2; source dataflow proves input identity, while graph symbols/order establish the executed operations.

## Narrow trial and lifetime

The candidate returned an explicit Q8Input from successful shared GatedDense, passed it locally through Moe/MoeExperts/GatedExperts, and called a prepared-input wrapper. It required verification, Q4_K matching gate/up shapes, rows exactly2/3, and identical source pointer/row count/K. The existing alternating Q8 slots preserve the input across exactly one shared-down quantization. The pointer never survives the Moe invocation. No cross-layer/request cache, new activation buffer, wait, copy, graph rebuild policy, or kernel arithmetic change was introduced.

The prepared wrapper allocates only existing group metadata scratch and calls the original grouped gate/up dispatch. Prompt14's rejected dispatch was not retested. Down/router/shared-expert kernels, MTP, lookup, sampler, PLE, and tuning were unchanged.

Trial files were CMakeLists.txt, native main/JSON diagnostics, executor.cpp/hpp, qfn_mmq.h/hip.cpp, activation_reuse.hpp, and the focused test. All restored/removed; trial copies remain under `benchmarks/prompt15/trial-source/`. The saved patch passes `git apply --check` against the restored tree.

## Correctness and build

Built only with `scripts/build.ps1 -SkipBundledTests`, installed native TheRock/HIP/gfx1151 toolchain. First attempt failed because the host test included device-only definitions; its declaration boundary was corrected, and the second build passed. No full suite, runtime/driver installation, Linux tools, or remote access.

Eight focused cases passed: rows2/3, M640/641, K2560, disjoint/shared/duplicate/inactive routes, guards, exact prepared bytes, baseline/reuse output, separate projections plus GPU SwiGLU reference, alternate-slot intervening preparation, and three graph replays per case with changed source contents at the same address. Test executed beside the production HIP DLLs.

## Production A/B

A: three requests in one loaded process. B: three plus the prescribed fourth because deviation exceeded3%. No excluded warmup; same diagnostic executable, gate selected before capture. All requests:1165 prompt/511 completion, seed12345, thinking OFF, qualified sampler/context/MTP/lookup/PLE/tuning. No second candidate or gain-confirmation pair; secondary new-code and code-edit runs skipped because primary did not win.

| Metric (median unless stated) | Baseline A | Reuse B |
|---|---:|---:|
| Decode tok/s | 35.920747 | 34.880628 |
| Individual decode tok/s | 36.023,35.921,35.839 | 33.155,34.040,35.721,35.875 |
| Prefill tok/s | 1336.186 | 1222.627 |
| TTFT ms | 959.343 | 1044.759 |
| 2-row completion ms/round | 45.052021 | 45.853840 |
| 3-row completion ms/round | 50.457030 | 52.183915 |
| 2-row / 3-row rounds per request | 132 / 120 | 132 / 120 |
| Whole verification completion ms | 12391.951 | 12717.812 |
| Proposal ms | 1389.838 | 1458.131 |
| MTP-only cycle ms | 14137.992 | 14571.997 |
| All speculative cycle ms | 14211.919 | 14645.663 |
| Rollback ms | 296.072 | 315.882 |
| Stream synchronization wait ms | 1590.778 | 1676.037 |
| Stream / event / device sync count | 361 / 0 / 0 | 361 / 0 / 0 |
| Unique eligible prepared batches/request | 11844 | 11844 |
| Actual compatible preparations/request | 23688 | 11844 |
| Avoided preparations/request | 0 | 11844 |
| WDDM GiB, every request | 86.628429 | 86.628429 |

Counters are replay-weighted: actual eager callbacks plus captured deltas applied once per graph launch, excluding capture construction. B reused6,204 row2 and5,640 row3 preparations. Both kept282 other-row Q4 baseline preparations. Missing templates/overflow zero. Verification graph captures6 first then10; replays506 first then516. All-profiled captures12,12,10(/10). No model/MTP reload within each group.

MTP drafted391/accepted253 (64.705882%); MTP output510; accepted/output0.4960784314. Lookup drafted6/accepted0, one round. Every hash matched:

`9b26a2ddd9518368939d4d29f107f398e793acd5f5cbdb926b2a9ffaa900c4f7`

Timing is **DEPENDENCY-BOUND GRAPH COMPLETION WALL TIME**, from before prefix through existing required completion. It includes host launch/PLE dependencies and is not pure GPU or isolated suffix time. No HIP event elapsed timings were used. Identical diagnostics in A/B; no separate causal profiling-overhead estimate.

B was noisy, and unchanged prefill/proposal timings also varied. This establishes no retention-worthy benefit, not a causal claim that reuse intrinsically slows kernels. Removing this work did not yield a measurable workload win; activation preparation is not established as the critical-path bottleneck. No further tuning performed.

## Graph-node accounting

| Shape | Baseline prefix/suffix nodes | Candidate expected prefix/suffix |
|---|---:|---:|
| 2-row | 39 / 1690 | 38 / 1644 |
| 3-row | 40 / 1740 | 39 / 1694 |

Baseline is the saved Prompt13.2 dump. Candidate counts are **source-derived expectations**, not a new whole-production graph-node dump. All quantize nodes484→437 per complete round; copies unchanged7/8. Real production replay compatibility and avoided-operation counts were measured above. Fewer nodes did not determine retention.

## Restoration and next step

All123 original file hashes match, including production executable/configuration and unrelated dirty/untracked work. Prepared Gufo sources also restored. Both task-started engines stopped. No new persistent allocation; WDDM stable over seven requests. Host5GiB guard stayed enabled; post-request available memory7.31–9.73GiB. This is a bounded-run observation, not a long-run leak proof. Disposable `build` still contains trial objects; restored `dist` is production, and the standard build script clears the disposable cache.

Next mapped structural candidate: Q8_0 shared-expert down projection, `Executor::Moe -> Dense(l.shexp_down,s_.shexp_up) -> qfn_mmq_q8_0_dense_vec_preq -> qfn_q8_shallow_kernel`, rows2/3, M2560,K640. It contributes48 launches/round (prefixnode31, suffixlayer1node41; grid2560, block32). The existing qfn_q8_mmvq_reference is a possible later comparator. Attribute this exact shape before one separate matched experiment; it is not yet a proven bottleneck.

Artifacts: `prompt15-activation-quant-reuse-ab.json` contains full per-run metrics and checks; `prompt15/` retains raw responses, graph/source proof, build/test logs, trial source/patch/binaries and restoration proof. Local only.

PROMPT 15 STATUS: PASS
ACTIVATION QUANTIZATION REDUNDANCY FOUND: YES
ACTIVATION QUANTIZATION REUSE RETAINED: NO
PRODUCTION CONFIG CHANGED: NO
GITHUB/REMOTE UPDATED: NO
