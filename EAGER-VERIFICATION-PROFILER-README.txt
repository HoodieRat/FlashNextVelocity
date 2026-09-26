FlashNextVelocity benchmark-only eager verification profiler
2026-09-25

PURPOSE
-------
This cumulative patch preserves the existing compact sampled-verification,
settings persistence, anchor-only fallback, clean-build protection, and runtime
revision checks. It adds one diagnostic behavior only:

  When flashnext_profile=true and the profiler is in DECODE phase, target/MTP
  verification Forward calls bypass HIP graph replay and execute the exact same
  kernels eagerly so HIP event timing can attribute GPU time to:

    VERIFY TRUNK
    HC HEAD
    VOCAB PROJECTION
    COMPACT SELECT
    D2H

Normal chat and all non-profiled /v1 requests continue using HIP graphs.
Sampling, MTP policy, acceptance, residual correction, draft confidence,
quantization, QSA, PLE, model weights, and ordinary runtime graph behavior are
unchanged.

IMPORTANT
---------
The GENERAL decode tok/s printed by a profiled benchmark after this patch is a
DIAGNOSTIC eager-verification number. Do not compare it directly to normal
HIP-graph throughput. Use the substage timings to decide what to optimize next.

INSTALL
-------
1. Fully exit FlashNextVelocity, including the tray icon.
2. Extract this ZIP over the current project root and replace files.
3. Run BUILD.bat.
4. The build must print both:

   Benchmark-only eager verification profiler source verified.
   Native engine source revision verified: fnv-eager-verify-profile-v4; legacy compact fatal absent.

5. Launch dist\FlashNextVelocity.exe.
6. Keep MTP draft max = 6 and draft confidence = 0.70.
7. Run ONE normal Studio benchmark and send the entire report back.

EXPECTED BENCHMARK MARKERS
--------------------------
Verification execution         EAGER diagnostic (benchmark only; normal inference keeps HIP graphs)
Verification graph bypasses   <non-zero>
Verification GPU events       recorded

HC HEAD, VOCAB PROJECTION and COMPACT SELECT should now report attributable
(non-graph-hidden) GPU event timings when those stages execute.

RUNTIME REVISION
----------------
fnv-eager-verify-profile-v4
