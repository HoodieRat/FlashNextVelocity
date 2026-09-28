# Prompt 16 — Q8_0 shared-expert down projection

## Decision

**PASS: OPTIMIZATION EVALUATED AND REJECTED.** One existing MMVQ path was tested for the exact shared-expert down operation at target verification rows 2/3. Median decode changed **36.610121 → 36.536436 tok/s (−0.2013%)**. Small dependency-bound completion differences did not produce an end-to-end win. Original shallow dispatch and production executable restored; no commit.

Starting/final HEAD: `b58deeb81e19b250222fe259d223339c8634543a`, branch `master`. Local rollback branch: `backup/pre-prompt16-q8-shared-down`.

**GitHub was never contacted.** No fetch/pull/push, remote branch/tag/release/settings/API change, upload or PR. Local refs, index, Git config and status match the snapshot taken after creating the rollback branch. `origin/main` and `production-qualified-2026-09-27` are absent locally before/after; no remote readback is claimed. The frozen backup was untouched by this task.

## Exact operation and orientation

Source: `Executor::Moe -> Dense(l.shexp_down, s_.shexp_up, s_.shexp_out) -> Quantize -> Dense(Q8Input) -> qfn_mmq_q8_0_dense_vec_preq`.

- Input is FP32 `[N,640]`, produced by shared gate/up activation. Existing preparation yields row-major Q8_1 blocks; K is padded to 1024, stride 32 blocks/token. Only 20 blocks contain the logical 640 values.
- Output is FP32 `[N,2560]`. M=2560 is output channels, K=640 is reduction/input width, N=2 or 3 is tokens. Kernel `rows` means M, not token rows.
- All 48 local GGUF shared-down tensors were inspected through metadata: `ne=[640,2560]`, type 8/Q8_0. Storage is 2560 output rows, contiguous K640, 20 Q8_0 blocks/row, 34 bytes/block, 680 bytes/row, 1,740,800 bytes/tensor. No transpose assumed.
- Baseline: `qfn_q8_shallow_vec -> launch_q8_shallow<2/3> -> qfn_q8_shallow_kernel<2/3,false>`.
- Selected because `N>=2 && N<=4 && (M>=2048 || K>=4096)` is true. HC-down special cases do not match.
- Geometry: grid `[2560,1,1]`, block `[32,1,1]`, one wave/workgroup, one output channel across both/all three tokens. FP32 accumulation, original per-lane K order and 32-lane reduction.
- One projection launch/layer, 48/verification round: 1 prefix plus 47 suffix. Canonical requests contain 132 row2 and 120 row3 rounds, totaling 12,096 eligible calls; another 288 calls at other verification widths remain baseline. All verification shared-down calls total 12,384.
- Saved graph mapping: prefix node 31 and suffix layer1 node 41 for both shapes. The audit includes every layer. Preparation and projection are separate kernels; only projection dispatch changed.

## Existing path selection

Selected: `qfn_q8_mmvq_reference -> mul_mat_vec_q8_dispatch -> launch_q8<2/3> -> mul_mat_vec_q8<2/3,false,1,false>`.

It accepts identical Q8_0 weights, Q8_1 input stride, FP32 output and dimensions, and is the local vector fallback for other shapes. It uses the same grid/block, K-block schedule (`lane/4`, increment 8), packed dot primitive, scales, accumulation sequence and reduction. Shallow hoists decoded weight loads outside its token loop; reference accesses them through the inlined dot helper inside the loop. Performance cannot be inferred from that difference.

Other paths inspected but not tested: wide Q8 matrix execution requires different K/token conditions; multi-wave batching targets more than 8 tokens; HC-down specialization is M320/K10240. None is an exact existing substitution here. No new kernel, threshold sweep, other K, or second candidate.

The candidate required explicit shared-down and target-verification scopes, M2560/K640, rows exactly 2/3, and a null gate pointer. Everything else retained production dispatch. Preparation was unchanged; Prompt14/15 experiments were not reintroduced. No extra buffer, copy, synchronization or graph rebuild behavior.

Trial files, all restored/removed: CMakeLists.txt; src/native/main.cpp and shared_down_json.hpp; overlay executor.cpp; overlay mmq/qfn_mmq.hip.cpp and shared_down.hpp; tests/q8_shared_down_test.hip.cpp.

## Build and correctness

**One successful build**, only `scripts/build.ps1 -SkipBundledTests`, existing native TheRock/HIP/gfx1151 toolchain and cached dependencies. No installs/full suite. Focused test ran beside production HIP DLLs.

Both exact shapes passed bitwise baseline/candidate/direct-MMVQ/unscoped-fallback comparison. Captured kernel identities proved selection and fallback separately from output equality. Three replays changed FP32 contents at the same address (zero/small/ordinary); prepared bytes and output guards remained intact. No HIP event timing used.

## Production A/B

A: 3 requests in one loaded runtime. B: 3 plus the required fourth because the initial median difference was within 1.5%; neither group triggered the noise rule. First requests included, no excluded warmup. Same diagnostic binary, process gate fixed before capture. No confirmation pair or secondary fixtures because gain was below 1% and negative.

All requests used 1165 prompt/511 completion tokens, seed 12345, thinking OFF, original four-shard target/context 131117, Q8_0+Latin+halo_greedy/max6, sticky 6→16 resetting each request, qualified sampler, tuning `none,+flag_waits`, PLE cached-overlapped-random/128 workers/lazy NVMe/8 MiB, and 5 GiB host guard.

| Metric, median unless stated | Baseline | MMVQ candidate |
|---|---:|---:|
| Decode tok/s | 36.610121 | 36.536436 |
| Individual decode tok/s | 36.355, 36.610, 36.665 | 36.530, 36.543, 36.437, 36.578 |
| Prefill tok/s | 1346.721 | 1317.430 |
| TTFT ms | 942.950 | 961.669 |
| 2-row completion ms/round | 44.482661 | 44.350922 |
| 3-row completion ms/round | 49.879778 | 49.851050 |
| Verification total ms | 12236.710 | 12217.933 |
| Proposal ms | 1334.194 | 1353.212 |
| MTP-only cycle ms | 13873.824 | 13903.492 |
| All speculative cycle ms | 13947.729 | 13974.292 |
| Rollback ms | 267.498 | 285.923 |
| Synchronization wait ms | 1529.781 | 1551.729 |
| Stream/event/device sync counts | 361/0/0 | 361/0/0 |
| Scoped baseline shallow calls | 12096 | 0 |
| Scoped candidate calls | 0 | 12096 |
| Candidate rows2 / rows3 calls | 0 / 0 | 6336 / 5760 |
| Other-width fallback calls | 288 | 288 |
| Exact-shape calls | 12096 | 12096 |
| WDDM GiB, every request | 86.628429 | 86.628429 |

Counters represent eager calls plus captured per-key deltas applied once per replay, excluding capture construction. No missing templates/overflow. Verification captures: 6 first request then 10; replays: 506 then 516. All-profiled captures: A12,12,10; B12,12,10,10. Model and MTP loaded once per group. No extra synchronization.

Graph substitution is one existing kernel for one kernel. Source predicts unchanged counts (2-row prefix/suffix 39/1690; 3-row 40/1740); these are historical baseline counts, not a new candidate production dump. Focused kernel identities and production counters prove real graph execution.

Every request matched hash:

`9b26a2ddd9518368939d4d29f107f398e793acd5f5cbdb926b2a9ffaa900c4f7`

MTP drafts 391/accepted 253, acceptance 64.705882%, MTP output 510, accepted/output 0.4960784314. Lookup drafted 6/accepted 0, one round. No output/speculative-semantic difference.

Timing is **DEPENDENCY-BOUND GRAPH COMPLETION WALL TIME**, before prefix through existing required completion. It includes host launch/PLE dependencies, not pure GPU or isolated suffix time. Same diagnostics in A/B; no separate causal profiler-overhead estimate.

The 2-row/3-row completion changes were only 0.2970%/0.0576%; verification total fell slightly while MTP-cycle increased. This is no demonstrated end-to-end gain, not proof the reference kernel is intrinsically slower. The specified noise/rejection rule applies. No further kernel optimized.

## Restoration and next step

All **123 original file hashes** match, including executable/configuration and unrelated work. Prepared Gufo sources and local Git state/index preserved. Saved trial patch passes `git apply --check`. Both task engines stopped. WDDM stayed 86.628429 GiB over 7 requests; post-request host memory 8.47–10.00 GiB. No candidate allocation added; 5 GiB guard unchanged. Bounded-run observation, not a long-run leak proof.

Production `dist` is restored. Disposable `build` retains trial objects; standard build clears it. Trial source/patch/binaries, tests, raw requests, source/GGUF audit and restoration evidence remain under `benchmarks/prompt16/`, local only.

New-code and sticky code-edit sanity: **not run**, primary did not win.

Next mapped structural candidate: FP32 MoE epilogue, `Executor::MoeExperts -> MoeEpilogueKernel`, compared in a separate task with existing `MoeEpilogueVec4Kernel<float>` for target verification rows 2/3. Expert output `[N][10][2560]`, router gate stride 513; scalar grid `[N,10,1]`, vector `[N,3,1]`, both block 256 and 48 launches/round. Fewer blocks may reduce parallelism; no bottleneck or expected win established. Require bitwise and real graph A/B. No epilogue edits/tests performed here.

PROMPT 16 STATUS: PASS
Q8 SHARED-EXPERT DOWN CANDIDATE RETAINED: NO
PRODUCTION CONFIG CHANGED: NO
GITHUB/REMOTE UPDATED: NO
