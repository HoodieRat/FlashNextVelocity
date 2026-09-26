# FlashNextVelocity Grok handoff

This is a CLEAN, REBUILDABLE SOURCE SNAPSHOT of the current working project.

Generated dependencies/build output are intentionally excluded. BUILD.bat and
the project scripts are expected to recreate them.

## Current runtime state
- Windows / gfx1151
- Qwen3.8 Flash-Next
- v1.0.10 async halo pipeline
- per-HC-stream MTP hidden normalization
- halo-greedy MTP
- Top-64/128/256 exact compact verification
- GPU-retained verification frontier with lazy full-row D2H
- context lookup
- Gufo overlay is project-owned and re-applied during build

## Last known benchmark baseline
- Prompt tokens: 1165
- Completion tokens: 493
- Decode: 33.46 tok/s
- Prefill: 1150.22 tok/s
- MTP max: 3
- MTP confidence: 0.00
- MTP-only acceptance: 66.6%
- MTP avg proposed width: 1.37
- MTP verify: 48.34 ms/round
- MTP verify: 25.247 ms/output
- Async halo chains: 246
- Retained frontier downloads: 0
- hipStreamSynchronize: 635
- Sync wait: 27.412 ms/output
- Depth 1: 156 cycles, 32.36 tok/s, 66.7% acceptance
- Depth 2: 88 cycles, 37.33 tok/s, 66.5% acceptance
- Depth 3: 2 cycles, 36.96 tok/s, 66.7% acceptance

Important:
Synchronization wait includes queued GPU execution; it is not all removable host idle time.

## Immediate next optimization target
Re-evaluate adaptive MTP depth economics for the asynchronous pipeline.

Do not blindly force depth 2.
Instrument expected cost/yield for depths 1/2/3 and adapt from measured recent
acceptance/prefix survival while preserving exact target semantics.

## Preserve
Do not regress:
- exact target sampling/output semantics
- Top-64/128/256 compact verification
- full-vocabulary fallback correctness
- rejection/frontier correctness
- per-HC-stream MTP normalization
- async GPU-feedback halo chain
- GPU-retained verification frontier
- context lookup
- gfx1151/Windows build
- tests

## Build workflow
1. Read scripts/build.ps1 and overlay scripts before changing Gufo integration.
2. Make durable changes in project-owned source/overlay files.
3. Do not make the only copy of a fix inside generated .deps/gufo.
4. Run BUILD.bat.
5. Benchmark against 33.46 tok/s.
6. Keep only changes that improve performance, correctness, or useful diagnostics.

## Intentionally omitted
- .deps
- .cache
- build output
- generated native binaries/DLLs
- downloaded archives
- model GGUFs
- stale log history
- Serena machine indexes/caches
- old patch payload/backups

Serena project configuration and memories are retained when present; its indexes
should be rebuilt in the new workspace.