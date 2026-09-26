FlashNextVelocity v1.0.9 ROOT-OVERLAY PATCH

BASE REQUIRED: v1.0.8 project (the previous root overlay already applied).

APPLY
1. Close FlashNextVelocity Studio / engine.
2. Extract this ZIP directly over the project root.
3. Replace existing files when prompted.
4. Run BUILD.bat.
5. Start Studio and run the same benchmark.

PRIMARY FIX
- Qwen3.8 Flash-Next MTP hidden-state RMSNorm now runs independently per hyper-connection stream instead of across the full HC*hidden row.
- Applied to single-session ROCm, batched ROCm, and the pinned Gufo scalar reference oracle.
- Runtime revision: fnv-mtp-hc-groupnorm-v11.
- v1.0.8 adaptive exact Top-64/128/256 verification is retained.

VERIFY IN BENCHMARK OUTPUT
MTP hidden normalization: per-HC-stream

Recommended comparison settings: keep your benchmark settings identical, with MTP draft confidence 0.00. Draft max 3 is currently the cleanest baseline from your six-run sweep.
