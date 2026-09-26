FlashNextVelocity - Exact Compact Sampled MTP Verification Patch
Date: 2026-09-25
Target: current FlashNextVelocity Studio v1.0.0 project / pinned Gufo 9cad13974cf6da0cd3674b4e0a88b14b7e4a2908

WHAT THIS PATCH DOES

1. Removes the redundant full verification-row host copy for single-session verification.
   - Full fallback rows remain in Executor pinned host memory.
   - Session verification reads synchronized pinned rows directly.
   - Batch/concurrent verification keeps the existing per-session buffer path.

2. Adds exact compact sampled verification for eligible requests.
   - Existing target/trunk forward is unchanged.
   - Existing full-vocabulary output projection is unchanged.
   - Intermediate target rows transfer raw GPU Top-256 only:
       256 original vocabulary IDs + 256 raw F32 logits.
   - The final verification/frontier row remains a complete full-vocabulary F32 row.

3. Exact eligibility is intentionally narrow.
   Compact verification is enabled only when:
   - sampled MTP is active,
   - top_k > 0,
   - max(top_k, min_keep, 1) < 256,
   - repeat_penalty >= 1.0,
   - frequency_penalty == 0,
   - presence_penalty == 0.

4. Exactness certificate.
   - Apply the authoritative repeat-penalty semantics to the raw Top-256.
   - Determine post-penalty top-k.
   - Require the adjusted kth-best score to be STRICTLY GREATER THAN the raw
     256th-best boundary score.
   - Equality/tie/lower score falls back to the existing full verifier.
   - Because repeat_penalty >= 1 only demotes repeated tokens, a successful
     certificate proves that no unseen token can enter the post-penalty top-k.

5. Original vocabulary IDs are preserved through target sampling.
   - A new DistributionMapped helper keeps original token IDs through penalty,
     top-k, top-p, min-p, temperature, normalization, and tie ordering.
   - This avoids the compact-index tie-ordering error that would otherwise make
     the shortcut subtly non-identical to the full target sampler.

6. Rejection/frontier behavior is unchanged in meaning.
   - Compact verification computes the exact residual correction token.
   - On rejection, existing Executor::Rollback downloads the already-computed
     full target row needed as the new frontier while restoring state.
   - No target forward is recomputed.
   - sampler.DeferSample(correction) is preserved.
   - If all proposals are accepted, the already-downloaded complete final row
     is used as the frontier and Rollback keeps its no-extra-download fast path.

7. Full sampled verification remains intact.
   - Unsupported sampler -> full path.
   - Certificate failure -> download only that existing target row, then use
     the old full verifier.
   - Greedy verification behavior is unchanged.

8. Benchmark-only verification profiling is added.
   Substages:
   - VERIFY TRUNK
   - HC HEAD
   - VOCAB PROJECTION
   - COMPACT SELECT
   - D2H
   - CPU ACCEPT / RESIDUAL

   Counters:
   - verify output rows
   - full rows D2H
   - compact rows D2H
   - verification D2H bytes
   - compact candidates transferred
   - certificate successes
   - certificate fallbacks
   - unsupported-sampler fallbacks
   - rejection frontier downloads

   HIP-event substage timing is eager-only. Graph replay is explicitly marked
   incomplete instead of pretending those event timings cover replayed work.
   Transfer/certificate counters are recorded outside the graph host body so
   they remain complete across replay.

9. Gufo changes are durable.
   - scripts/apply-gufo-overlay.ps1 reapplies project-owned Gufo source files
     after the normal pinned-source preparation step on every build.
   - It also injects the narrow mapped sampler helper into pinned sampling.cpp.
   - This avoids changing prepare-gufo.ps1's hash and avoids a giant numbered
     patch stack.

VALIDATION DONE IN THIS ENVIRONMENT

- mtp_sampling.hpp: C++23 syntax-only compile passed.
- DistributionMapped method: standalone syntax-only compile passed.
- Randomized mathematical equivalence test:
    5,000 sampled configurations
    4,503 certified compact cases matched the full distribution exactly within
    floating comparison tolerance and with identical token support/order.
    497 cases failed the strict certificate and correctly selected fallback.
- Overlay copies were byte-compared with the working Gufo source files.
- Required symbols/counters/guards were source-audited.

A Windows ROCm/MSVC build cannot be executed in this Linux sandbox. BUILD.bat
is therefore the authoritative compile/runtime validation on the target PC.

INSTALL

Extract this ZIP directly over:
C:\FlashNextVelocity-Studio-v1.0.0\FlashNextVelocity-Studio-v1.0.0

Allow replacement of files, then run BUILD.bat.

DO NOT change the normal benchmark settings just to validate the patch.
For the user's current sampled benchmark configuration, top_k=40 and
repeat_penalty=1.05 are eligible for compact verification.

WHAT TO LOOK FOR AFTER BUILD

A benchmark should now show a "Verification substages" section. With the
current top_k=40 / repeat_penalty=1.05 sampled setup, expect:
- compact rows D2H > 0,
- compact candidates transferred > 0,
- certificate successes > 0 when the strict proof succeeds,
- full rows D2H roughly one final frontier row per speculative cycle plus rare
  certificate/rejection downloads,
- verification D2H bytes materially below the old all-full-row path.

Do not judge the patch only by acceptance percentage. Compare decode tok/s,
target verification time, D2H bytes, certificate success/fallback counts, and
draft-depth behavior on the same workload.
