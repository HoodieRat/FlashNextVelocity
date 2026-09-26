# FlashNextVelocity v1.0.10 - Async Halo / Lazy Frontier Pass

## Why this pass
The v1.0.9 benchmark showed correct per-HC-stream MTP normalization and much higher proposal quality, but the profiler recorded hundreds of `hipStreamSynchronize` calls and roughly 220 MB of verification D2H traffic. The largest avoidable host-side costs were repeated draft-step synchronization and unconditional full-vocabulary final-frontier transfer.

## Implemented

### 1. GPU-feedback halo-greedy MTP chain
When sampled MTP uses `halo-greedy` with `draft_confidence=0.00`, the predictor chain now keeps each argmax on device. Step N+1 consumes step N's GPU token directly. Draft IDs are accumulated in a contiguous device buffer and copied to pinned host memory once at chain end, followed by one synchronization.

The ordinary path remains intact for distribution proposals, confidence-gated sampled MTP, tracing, candidates, batched paths, and other modes.

### 2. GPU-retained target frontier
Top-64/128/256 exact verification still transfers compact candidate rows required by the CPU sampler/certificate logic. The final full-vocabulary target row is copied device-to-device into session-owned storage instead of being downloaded every successful speculative round.

If the next compact frontier cannot certify complete support, the full row is downloaded lazily. Snapshot creation also materializes the authoritative retained row only when necessary.

### 3. Deferred device-only rollback synchronization
Rollback operations that only restore GPU recurrent/KV/PLE state no longer block the host. They remain ordered on the same nonblocking HIP stream. A rollback that returns a host frontier still synchronizes before the caller consumes it.

## Correctness invariants retained
- Exact target sampling semantics.
- Full-vocabulary fallback on compact certificate miss.
- Rejection frontier download before CPU use.
- Per-HC-stream MTP hidden RMSNorm.
- Gufo calibrated MTP cost policy.
- Top-64/128/256 exact compact verification selector.
- Recurrent state and rollback ordering.
- Snapshot frontier correctness.

## Benchmark interpretation
A lower synchronization count is useful, but synchronization time includes time spent waiting for real GPU work. Therefore v1.0.10 is expected to remove host round trips and D2H traffic; it does not claim that all previously attributed sync milliseconds disappear. If target verification remains dominant after this pass, the next high-value target is the verification/output-projection compute path itself.
