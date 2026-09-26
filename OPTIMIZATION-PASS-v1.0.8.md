# FlashNextVelocity v1.0.8 top-wins optimization pass

Baseline: user-supplied v1.0.6 project ZIP, pinned Gufo `9cad13974cf6da0cd3674b4e0a88b14b7e4a2908`.

This pass intentionally does **not** use the abandoned forced-depth-2 v1.0.7 experiment. It keeps the current adaptive policy, sampler settings, model quantization, context size, and exact target-sampling rules.

## Implemented now

### 1. Deferred exact verification frontier D2H

The v1.0.6 compact sampled-verification path already selected exact raw Top-256 candidates for each target row, but `Executor::ForwardBody` still copied the **entire final vocabulary row to host on every compact verification pass**. The next decode step first attempted `SampleMtpFrontierCompact`; therefore that full transfer was often unused.

v1.0.8 removes that unconditional final-row copy. After a fully accepted compact cycle:

1. the exact Top-256 frontier remains available in pinned host memory;
2. the authoritative full target row remains resident in `s_.logits` on the GPU;
3. the next anchor first tries the existing exact compact certificate;
4. only if the certificate cannot prove the authoritative sample do we call `DownloadVerificationRow` and pay the full-vocabulary D2H + sync;
5. snapshots and the public `Logits()` accessor also materialize the deferred row before consuming it.

Rejection behavior is unchanged: rejection-frontier rollback still downloads exactly the authoritative row that becomes the next autoregressive frontier. Seeded RNG consumption is unchanged because the compact certificate path already performs the same sampler draw only when exact support is proven, and certificate failure performs no draw before the full-row fallback.

Telemetry now counts compact verification as compact bytes only. A later certificate fallback is counted as a full-row D2H at the point it actually occurs.

### 2. Preserve the current Gufo LLVM-23/gfx1151 W8A8 fix

The supplied v1.0.6 ZIP already contains the Gufo fix from commit `98641a6503da2ec5d6dbb1888ddc95f8a3e13b28`: one `__builtin_amdgcn_sched_barrier(0)` per W8A8 K block when `__clang_major__ >= 23`. Gufo reported that LLVM 23 otherwise hoisted the next K-block operands, caused gfx1151 register spills, and reduced occupancy. Their matched Qwen3.8-Flash-Next UD-Q4_K_XL pp2048 test reported about +5.1% prefill.

v1.0.8 keeps this source in the project-owned overlay and keeps the build gate that refuses to build if the fix disappears.

### 3. Remove the old-project ROCm fallback

`scripts/ensure-local-rocm.ps1` no longer contains a hard-coded `C:\flashfknworkalready\.deps\rocm` source. It can reuse a valid installed Windows ROCm SDK, or `BUILD.bat` downloads the pinned gfx1151 SDK into this project's `.deps\rocm`.

The build now explicitly rejects a ROCm helper that reintroduces the old-project path.

### 4. Repeatable A/B suite

`BENCHMARK-TOP-WINS.bat` / `scripts\benchmark-top-wins.ps1` adds fixed-prompt, fixed-seed profiled runs for:

- normal chat;
- coding;
- structured JSON;
- continuation prose;
- predictable/repetitive text;
- optional long-context ladder.

Every run stores prompt hashes, output hashes, effective sampling/thinking settings, runtime revision, per-repetition metrics, verification D2H rows/bytes, certificate success/fallback counts, and raw profile text. Comparison refuses mismatched settings or prompt hashes.

## Source-level strongest evidence

| Finding | Evidence | Decision |
|---|---|---|
| Full verification frontier was downloaded every compact cycle | v1.0.6 `executor.cpp`: after Top-256 transfer loop, a second `hipMemcpyAsync` copied `(n_logits - 1) * vocab` full row unconditionally | **Fixed in v1.0.8** |
| Exact compact fallback already existed | `FinishDecode`: certificate failure calls `DownloadVerificationRow(row)` before full verification | Reused, not redesigned |
| Rejection frontier already downloads the exact required row | `Rollback(..., logits)` copies `keep - 1` row while restoring state | Preserved |
| Old high-acceptance proposal semantics were deterministic draft top-token | v1.0.6 `halo_greedy` implementation and preserved halo-box source audit | Preserved as default; no new sampler change |
| LLVM-23 W8A8 spill issue has a proven gfx1151 fix | Gufo commit `98641a65...` | Already present; preserved and build-gated |

## Cross-project best-of audit

| Technique | Project/source | What it solves | FlashNextVelocity status | Action |
|---|---|---|---|---|
| gfx1151 W8A8 K-block scheduler fence | Gufo / pwilkin | LLVM-23 register spilling, low occupancy | Equivalent already present | Keep + gate |
| Deterministic MTP top-token proposal | halo-box/strix-llama.cpp lineage | Higher MTP proposal agreement on this model family | v1.0.6 `halo_greedy` | Keep; measure acceptance |
| Recurrent-state speculative rollback/checkpoints | halo-box / llama-halo-hybrid | Correct partial MTP acceptance | Gufo has explicit rollback snapshots/state restore | Do not transplant another checkpoint architecture |
| Pooled QSA block-key cache | llama-cpp-rdna-boosts | Avoid rebuilding long-context indexer representation each token | Gufo already maintains `block_k` pooled indexer cache | Measure at depth; no duplicate port |
| Sparse selected-block FA/QSA | rdna-boosts / llama-halo-hybrid | Avoid attending full cache at long context | Gufo already has sparse selection + selected-block attention kernels | Benchmark 32K/64K/131K before changing |
| Managed lazy/on-direct PLE reader | rdna-boosts | Avoid resident giant PLE table / page pressure | Windows runtime already uses lazy mapped model access plus separate PLE I/O path | No Linux direct-I/O transplant |
| Grouped decode matvec / small-K kernels | halo-box / llama-halo-hybrid | Launch + bandwidth reduction for narrow projections | Gufo already has specialized Qwen3.8 kernels; implementation differs | A/B individual shapes before porting |
| GPU-side compact target verification | Gufo + project overlay | Avoid full-vocab host sampling for every draft row | Implemented; v1.0.8 removes remaining unconditional final full row | **Top decode win in this patch** |
| DFlash2 selector/lattice | pwilkin/strix-halo | Alternate drafter economics | Different model/drafter semantics | Do not replace native MTP head |
| WSL/DXG memory placement | strix-alloy-wsl2 | Large-model residency under WSL | Violates native-Windows requirement | Not applicable |

## Expected benchmark effect

No token/s result is claimed without running on the target gfx1151 machine. The deferred-frontier change should reduce verification D2H bytes and host synchronization work whenever compact certificates succeed. The effect on sustained decode depends on how much of the previous cycle time was attributable to that transfer versus GPU trunk/projection work.

The W8A8 barrier primarily improves prefill on LLVM 23+; it was already in the supplied v1.0.6 baseline, so it should not be credited as a new v1.0.8 A/B gain.

## Remaining high-value work

1. Run the new suite and inspect `full_rows_d2h`, `d2h_bytes`, `certificate_successes`, verification projection time, and sustained 64-token windows.
2. Continue the old halo-box vs current Gufo hidden/recurrent MTP-state audit if `halo_greedy` still does not recover historical acceptance.
3. Profile verification width 1/2/3/4 on gfx1151 and attack the largest GPU substage, not the sync wrapper.
4. Run QSA depth ladder at real occupied context before touching shallow-context QSA code.
5. A/B grouped/small-K projection kernels only where Gufo's current implementation loses on measured gfx1151 shapes.
