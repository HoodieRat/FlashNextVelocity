# Prompt 13.1 — PASS B: definitive fallback established

**Decision:** standard HIP events reliably time the deterministic eager/synthetic graph workload, but fail for actual production verification graphs on this installed runtime. Use **DEPENDENCY-BOUND GRAPH COMPLETION WALL TIME** for production attribution. Pure GPU verification time is unavailable from the tested event pattern.

- Starting/final HEAD: `b58deeb81e19b250222fe259d223339c8634543a`. **No commit.**
- All trial production changes and the trial EXE were restored; all 123 original file hashes, Git status, index, local refs and configuration match. Only local benchmark evidence/source/patches remain.
- No GitHub contact or remote writes. Remote-tracking refs did not change; no remote branch/tag/release operation occurred. The permitted local rollback branch was created.

## Installed identity and API contract

- Production module: `<workspace>\dist\engine\amdhip64_7.dll`; file version 10.0.3581.0.
- HIP runtime/driver/header integer version: **71526333** (7.15.26333), header hash `6b0e43f341`.
- TheRock package **10.0.0rc4**, ROCm 10.0.0, commit `16adc4d875fd4f65ea23c7c84e1c66706fde3047`.
- AMD clang 23.0.0git, compiler commit `8f497e0992fb7513f7f78a6f6b6f1056c375e961`; production MSVC 14.44 host environment; gfx1151 / Radeon 8060S.
- Headers define every requested API/flag. All seven event API exports are present. `hipEventRecordExternal` is declared but was not needed or exercised; no events were captured inside a graph.
- Standard creation: `hipEventCreateWithFlags(..., hipEventDefault)`; timing remains enabled. Precision combination `hipEventDisableSystemFence | hipEventReleaseToDevice` (`0x60000000`) is rejected with **hipErrorInvalidValue (1)**. No precision kernels were measured; no further flag sweep.
- An initial standalone launch resolved the older System32 HIP 70260201. That run is explicitly excluded. The accepted run launched beside production DLLs and verified the loaded module; production requests independently verified the same DLL/version.

## Prompt 13 failure-code analysis

The saved Prompt 13 artifacts contain aggregate invalid-span counts only: 139 and 158 failures in two separate requests. They do **not** preserve exact return codes or failed raw durations, so the original error class cannot be reconstructed. The new experiment records those fields explicitly; it does not retroactively invent codes for Prompt 13.

## Standalone benchmark

Deterministic unsigned-integer kernel: 4096 threads × 65536 recurrence iterations, full output checked against a CPU result. Each trial records start → work/graph launches → stop on one nonblocking stream, synchronizes the stop once, queries both events, reads elapsed, then checks output. No synchronization between graph launches. Unique pairs are destroyed only after consumption; the 4-pair pool is reused only after completion and consumption.

| Standard events | Unique pair median GPU ms | Pool median GPU ms | Result |
|---|---:|---:|---|
| Eager (5 trials/lifecycle) | 0.459500 | 0.482500 | PASS |
| One graph (5 trials/lifecycle) | 0.462401 | 0.465300 | PASS |
| Batch 1 (3 trials/lifecycle) | 0.462900 | 0.462600 | PASS |
| Batch 8 (3 trials/lifecycle) | 3.575499 | 3.567400 | PASS |
| Batch 32 (3 trials/lifecycle) | 14.166500 | 14.153499 | PASS |
| Batch 128 (3 trials/lifecycle) | 56.706478 | 56.727459 | PASS |

All **44 measured standard-event trials** (plus 12 warmups) passed timing API and correctness checks. After stop completion, all queries and elapsed reads returned success. GPU durations were finite/positive and below synchronized QPC wall time. For batches 32/128, GPU time settles near **0.443 ms/launch**. Unique and pooled graph behavior agree. Raw JSON records every API return, including expected pre-completion NotReady queries, creation/destruction, and QPC wall values.

## Actual production experiment

One warmup and two measured canonical requests; exact target/Q8_0/Latin/MTP/sticky lookup/sampler preserved. Whole existing verification prefix/suffix launches only; 8 event pairs (16 events), safely reused after both completion queries and elapsed consumption. No internal event nodes, graph-key/topology changes, or new synchronizations.

| Request | Decode tok/s | TTFT ms | Replays | Positive API samples | Negative durations |
|---|---:|---:|---:|---:|---:|
| warmup | 35.794 | 1528.917 | 506 | 430 (84.98%) | 76 |
| measured1 | 36.172 | 1183.641 | 516 | 402 (77.91%) | 114 |
| measured2 | 36.748 | 954.348 | 516 | 405 (78.49%) | 111 |

**Exact observed failure:** every start/stop record, both post-completion event queries, and every elapsed read returned **hipSuccess (0)**. Nevertheless, measured requests contained **114/111 negative elapsed durations**. No NotReady, invalid-handle/value, nonfinite or zero duration occurred. The failures are in verification suffixes; many positive suffix samples are also implausibly close to zero. Positive-return counts are therefore not a certification of those samples. The ≥99% validity requirement fails; no GPU total or ms/round is reported.
This rules out an observed premature read or pending-event overwrite under the documented API contract. It identifies a production-graph timestamp contract failure, not its internal driver implementation cause. Synthetic graphs pass; blanket claims that all Windows HIP graph timing fails would be unsupported.

### Shape-dependent event failures (two measured requests)

| Rows / draft depth | Piece | Samples | Negative durations |
|---|---|---:|---:|
| 2/1 | prefix | 264 | 0 |
| 2/1 | suffix | 264 | 97 |
| 3/2 | prefix | 240 | 0 |
| 3/2 | suffix | 240 | 126 |
| 4/3 | prefix | 2 | 0 |
| 4/3 | suffix | 2 | 0 |
| 5/4 | prefix | 8 | 0 |
| 5/4 | suffix | 8 | 2 |
| 7/6 | prefix | 2 | 0 |
| 7/6 | suffix | 2 | 0 |

## Selected fallback: DEPENDENCY-BOUND GRAPH COMPLETION WALL TIME

Use Windows QPC before the existing first verification-piece submission and immediately after its existing required completion. Record coherent-flag wait duration separately and group by verification row count. Capture count distinguishes one-time graph construction. Prefix/suffix per-launch intervals overlap and must not be summed; the one-per-round intervals below do not overlap.
Measured medians: **12455.858 ms** across 258 verification rounds (**88.87%** of decode); **11175.289 ms** in existing verification flag waits; **1047.609 ms** in graph launch APIs. These are overlapping host views, not pure GPU costs or automatically removable overhead.

| Rows / depth | Rounds | Replays | Captures | Completion wall ms | ms/round | Decode share |
|---|---:|---:|---:|---:|---:|---:|
| 2/1 | 132 | 264 | 2 | 5939.500 | 44.996 | 42.38% |
| 3/2 | 120 | 240 | 2 | 6122.577 | 51.021 | 43.68% |
| 4/3 | 1 | 2 | 2 | 67.026 | 67.026 | 0.48% |
| 5/4 | 4 | 8 | 2 | 249.561 | 62.390 | 1.78% |
| 7/6 | 1 | 2 | 2 | 77.194 | 77.194 | 0.55% |

Existing synchronization categories (measured median):

- other existing stream sync: 0 calls, 0.000 ms.
- MTP chain D2H readiness: 257 calls, 1157.579 ms.
- rollback frontier dependency: 104 calls, 146.348 ms.

Stream synchronization count remains **361**, device/event synchronizations **0**. There are 258 existing verification coherent-flag completion waits. The fallback adds no wait. It includes required GPU completion, host API/capture costs and PLE dependency where applicable, and cannot identify internal kernel costs.

## Correctness, overhead, memory, and restoration

- All three requests: **1165 prompt / 511 completion tokens**, seed 12345; 391 MTP drafts, 253 accepted (64.706%); one lookup round, 6 proposals, 0 accepted. Graph and synchronization counts match the prior warm production path.
- Output SHA-256 unchanged: `9b26a2ddd9518368939d4d29f107f398e793acd5f5cbdb926b2a9ffaa900c4f7`.
- Measured median: **36.460 tok/s**. Versus historical Prompt 13 controls (34.345), observed delta is **+6.16%**. This is not a controlled overhead estimate or speed improvement. No contemporaneous OFF pair was added beyond the requested three requests, so an under-2% overhead claim is not established.
- WDDM: **86.628429 GiB** after every request; event count stays 16, peak pending 2, pending end 0, overflow 0. Model/MTP each loaded once; 5 GiB host guard unchanged. No long-run leak-freedom claim from three requests.
- Native build: first attempt hit a Windows DrawState macro collision in the diagnostic header; localized fix, second production-script build passed. No full test suite.
- **RGP CROSS-CHECK: NOT AVAILABLE.** Executables were not immediately discoverable; no installation, driver/toolchain change, GUI automation, WSL or Linux profiler was used.
- Production trial source and executable were rejected/restored. All original source/configuration/runtime hashes, Git status/index/refs and local Git configuration verified. Prepared Gufo executor restored. Only task-started PID was stopped. The disposable build cache contains trial build products; normal `scripts/build.ps1` clears it on the next build.

## Narrow next step

Investigate the production verification suffix at rows 2 and 3 (draft depths 1 and 2), using dependency-bound wall attribution. First isolate the event timestamp contract failure with a representative production suffix graph/node composition. Do not select HC/projection/QSA kernels or remove waits based on invalid GPU timing.

The established contract is host dependency attribution for the dominant 2-/3-row verification suffix path. The synthetic event lifecycle is reusable for synthetic timing, but it is **not** approved for production GPU attribution.

PROMPT 13.1 STATUS: PASS
HIP GRAPH GPU TIMING TRUSTWORTHY: NO
FALLBACK TIMING CONTRACT AVAILABLE: YES
PRODUCTION CONFIG CHANGED: NO
GITHUB/REMOTE UPDATED: NO
