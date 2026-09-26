FlashNextVelocity Studio v1.0.10 - Async Halo Pipeline

APPLY
1. Close FlashNextVelocity.exe and FlashNextVelocity.Engine.exe.
2. Extract this ZIP directly over the project root and replace files.
3. Run BUILD.bat.
4. Wait for: FLASHNEXTVELOCITY BUILD + RUNTIME VALIDATION PASSED.
5. Launch dist\FlashNextVelocity.exe.

FIRST A/B BENCHMARK
- MTP proposal mode: halo-greedy
- MTP draft confidence: 0.00  (required for the async halo fast path)
- MTP draft max: 3 for the first clean comparison
- Keep the rest of the sampling/context settings unchanged.

WHAT THIS PATCH TARGETS
- Chains halo-greedy MTP argmax steps on the GPU without a host synchronization between draft steps.
- Stores the draft IDs in a contiguous GPU chain and performs one bulk D2H + one stream sync at chain end.
- Retains the all-accepted target frontier on the GPU; full-vocabulary D2H happens only when the compact support certificate fails or a snapshot explicitly needs it.
- Device-only recurrent rollback stays ordered on the decode stream without a host synchronization.
- Keeps v1.0.9 per-HC-stream MTP normalization and v1.0.8 exact Top-64/128/256 compact verification.

EXPECTED TELEMETRY
- Runtime revision: fnv-async-halo-pipeline-v12
- MTP async halo chain: ON (GPU feedback; one chain-end sync)
- Verification frontier: GPU-retained + lazy full-row D2H
- Async halo chains > 0 when halo-greedy + confidence 0.00 is used.
- Full rows D2H should be limited primarily to rejection/certificate fallback paths rather than every all-accepted round.

This patch attacks host barriers and full-frontier transfers. It does not remove the target model's actual GPU verification compute, so measure sync count, D2H rows/bytes, proposal time, verification time, and tok/s together.
