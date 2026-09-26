FlashNextVelocity v1.0.5 - native Strix Alloy / Halogen ideas integration

Implemented
-----------
1. Native context-lookup speculative proposal lane.
   - Windows-native, no WSL/DXG/Linux hooks.
   - Searches committed context for the longest recent matching n-gram.
   - Reuses the existing Gufo target verification + rollback path.
   - Keeps the MTP recurrent block synchronized with a headless catch-up, but
     skips MTP proposal/head work for lookup-selected cycles.
   - Sampled decoding remains exact: lookup uses a one-hot proposal q and the
     existing p/q rejection + residual correction verifier.
   - Automatically falls back to normal MTP when no strong lookup match exists.

2. MTP economics telemetry.
   - MTP-only acceptance is separated from lookup acceptance.
   - Proposed width/round, emitted yield/round, target verify ms/round,
     target verify ms/output token.
   - Prefix survival by draft position.
   - Per-depth proposal/verification economics.

3. Windows memory guard.
   - Checks both available physical RAM and Windows commit headroom before load,
     after load, at request start, and periodically during generation.
   - Default floor: 8 GiB, configurable in Studio/settings.
   - This is a safety floor, not a VRAM-unlock mechanism.

4. Studio/config integration.
   - Context lookup and memory guard are first-class persisted settings.
   - /health exposes the active values so stale config cannot silently run.
   - Benchmark MTP acceptance now reports MTP-only acceptance; lookup metrics
     remain separately visible in the full report/JSON.

Defaults
--------
context_lookup: true
context_lookup_min_ngram: 3
context_lookup_max_ngram: 6
context_lookup_window: 32768
context_lookup_min_draft: 2
memory_guard: true
memory_guard_min_available_gib: 8.0

Intentionally not imported
--------------------------
- WSL2, DXG bridge, Linux mmap/proc hooks.
- Acceptance-EMA adaptive draft sizing (published negative result).
- 512-expert routing fast path (published near-zero result with HIP graphs).
- Reduced-vocabulary MTP head conversion. The published experimental path
  faulted during expansion and your current shared Q8_0 sidecar does not carry
  the required d2t/t2d mapping.

Build
-----
Run BUILD.bat. The build script now requires runtime revision:
  fnv-context-lookup-economics-v8
