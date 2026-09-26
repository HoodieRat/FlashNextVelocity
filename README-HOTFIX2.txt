FlashNextVelocity v1.0.9 HOTFIX2 - compact verification source repair

Apply:
1. Extract this ZIP directly over the current v1.0.9 project root.
2. Replace existing files.
3. Run BUILD.bat again.

What this fixes:
- Re-supplies the complete known-good 15-file Gufo overlay from the intended v1.0.9 state.
- Restores Top-64 / Top-128 / Top-256 exact compact verification kernel definitions, declarations, transfer modes, executor dispatch, and synchronized full final-frontier download.
- Retains the v1.0.9 per-HC-stream MTP hidden RMSNorm correction in executor.cpp and batch.cpp.
- Retains sampled-MTP 256-candidate support, adaptive exact verification width, halo-greedy proposal semantics, calibrated MTP costs, native context lookup, and gfx1151 LLVM 23 W8A8 scheduler-spill fix.
- Replaces the compact-verification build gate with direct implementation validation and detailed missing-marker errors.

This is a build/source-repair hotfix. Runtime revision remains fnv-mtp-hc-groupnorm-v11 / version 1.0.9.
