# FlashNextVelocity v1.0.6 optimization pass

This pass targets the acceptance regression before lower-value kernel work.

## Source-level finding

The preserved halo-box runtime (`halo-box/strix-llama.cpp` at `7449a0fe...`) generated Qwen3.8-Flash-Next MTP drafts by taking the MTP head's top token, then verified that deterministic draft by sampling the authoritative target distribution at each position. The current Gufo lineage instead sampled a stochastic proposal distribution from compact MTP candidates and used p/q rejection/residual verification.

Both approaches can preserve the target distribution, but they have different acceptance economics. At the low-temperature settings used by FlashNextVelocity, deterministic MTP argmax can have much higher overlap with the target sample when the two heads agree on the dominant token. This is the first concrete old-X/current-Y proposal-path difference found during the 90%+ versus ~60-70% acceptance investigation.

## Implemented

- `mtp_proposal_mode = "halo_greedy"` (default):
  - raw MTP argmax proposal, matching the old runtime's draft semantics;
  - deterministic one-hot proposal; no private proposal RNG;
  - exact target sampling decides acceptance;
  - rejected target draw is deferred as the next anchor, preserving the authoritative sample stream;
  - when draft confidence is `0`, the proposal path requests only the MTP argmax instead of Top-256 candidates.
- `mtp_proposal_mode = "distribution"`: retains the existing Gufo Top-256 stochastic p/q path for A/B testing.
- Draft confidence is separated from proposal q probability. Halo mode computes confidence from the raw Top-10 MTP logits, while q remains exactly one-hot.
- Compact target verification works for both proposal modes and falls back without consuming RNG if its support certificate cannot prove correctness.
- `prefill_batch` now controls Gufo's real native executor `max_batch` / prefill scratch. This is intentionally not presented as a llama.cpp `ubatch` knob.
- Studio settings, live config verification, health output, metrics, effective-settings report, build gates and native unit tests were extended for both settings.

## What was deliberately not changed

- QSA kernels: current measurements show QSA is not a decode bottleneck.
- PLE/NVMe path: worker read time is asynchronous; actual `WaitRead` is currently small.
- MTP static cost curves: changing proposal semantics changes the cost/acceptance surface. Recalibrate after collecting post-patch depth 0-4 timings rather than baking the old ~69% run into policy.
- Adaptive-depth algorithm: acceptance-only adaptive policies were already shown to lose on this model. Existing Gufo cost+acceptance policy remains in place.
- Sampling correctness: no approximate target distribution or acceptance shortcut was introduced.

## First A/B run

For the cleanest acceptance comparison, temporarily disable Context Lookup and keep all sampling settings identical. Run the same prompt twice:

1. `mtp_proposal_mode = "halo_greedy"`
2. `mtp_proposal_mode = "distribution"`

Keep `draft_max`, draft confidence, temperature, top-p/top-k/min-p and repeat penalty identical. Compare MTP-only acceptance, prefix survival, target verify ms/output, depth distribution and decode tok/s.

If halo-greedy restores acceptance materially, the next pass should calibrate Windows MTP costs with that mode and then attack target-verification/synchronization. If it does not, the remaining high-value comparison is the MTP hidden/recurrent-state trajectory against the preserved halo-box runtime.
