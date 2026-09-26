FlashNextVelocity normal-graph + Windows C1 performance patch
Runtime revision: fnv-normalgraph-c1-llvm23-v6
Date: 2026-09-25

Apply over the current FlashNextVelocity project after fully exiting the desktop/tray app, then run BUILD.bat.

What this patch changes

1. Restores normal HIP graph replay for verification benchmarks.
   - Removes the benchmark-wide eager-verification bypass from the previous diagnostic build.
   - Normal benchmark throughput is once again measuring the same graph-replay execution model used by normal inference.

2. Adds a deterministic Windows/gfx1151 C1 shallow MTP cost profile.
   - Applies only to one active request at context <= 2048.
   - Derived from the valid normal-graph Windows measurements:
       d0 46.93 ms
       d1 54.59 ms
       d2 65.91 ms
       d3 81.82 ms (small sample)
       d4 101.77 ms (small sample)
   - d5-d7 remain conservative monotonic extrapolations.
   - Does NOT force a draft depth. The existing adaptive controller remains in charge and can choose 0..configured max.
   - Does NOT change proposal probabilities, target sampling, p/q acceptance, residual correction, RNG, rollback, or model arithmetic.
   - Purpose: stop bootstrapping shallow Windows C1 policy from Linux-derived relative cycle costs and make the controller value the measured fast d1/d2 shapes correctly.

3. Integrates Gufo upstream perf commit 98641a6503da2ec5d6dbb1888ddc95f8a3e13b28 for LLVM 23/gfx1151.
   - Adds the compiler-conditional K-block scheduler barrier to W8A8BlockedWmmaGEMMKernel.
   - This is active on the project's ROCm 10.0 / Clang 23 toolchain.
   - Upstream reported elimination of LLVM-23 VGPR spills and 45-51% less time per affected W8A8 call, with pp2048 1459.99 -> 1535.02 tok/s (+5.1%) on Strix Halo.
   - The barrier is disabled on LLVM < 23.
   - This primarily improves affected W8A8 matrix paths; do not interpret the upstream prefill result as a guaranteed decode gain.

4. Preserves prior fixes.
   - Exact compact sampled verifier + full correctness fallback.
   - Anchor-only confidence-stop fix.
   - Settings persistence/read-back verification.
   - Clean native build to prevent stale Ninja objects.
   - Project-local ROCm/gfx1151 pin.

Expected build verification lines

  Normal HIP-graph verification path restored.
  Windows C1 MTP cost profile source verified.
  LLVM 23 gfx1151 W8A8 scheduler-spill fix verified.
  Native engine source revision verified: fnv-normalgraph-c1-llvm23-v6; legacy compact fatal absent.

After build

Keep the validated settings:
  MTP draft max = 6
  MTP draft confidence = 0.70

Run one normal benchmark and send the full result. In EFFECTIVE SETTINGS / MTP, confirm 6 / 0.70. The previous "EAGER diagnostic" line must be gone. The report will also show:
  MTP cost profile: FNV Windows C1 shallow-v1

The target of this pass is to restore at least the prior normal-graph baseline while giving the adaptive sampled controller a locally measured Windows C1 cost prior and applying the new upstream LLVM-23 W8A8 fix. Performance gain must be judged from the normal benchmark; it is not hard-coded or claimed in advance.
