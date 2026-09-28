# FlashNextVelocity diagnostic checkpoint — Prompts 13–16

This archive preserves measurements and rejected experiments performed against
qualified production commit `b58deeb81e19b250222fe259d223339c8634543a`.
It introduces no production optimization. The qualified main branch, production
tag and release remain the reference build.

| Task | Result | Retained production change |
|---|---|---|
| 13 | Internal GPU event profiling rejected; wall waits usable | None |
| 13.1 | Synthetic graph timing validated; production GPU timing unreliable | None |
| 13.2 | Production topology mapped; dependency-bound wall contract established | None |
| 14 | Q4 small-row dispatch: 36.027 → 35.773 tok/s | None |
| 15 | Activation reuse: 35.921 → 34.881 tok/s | None |
| 16 | Shared-down MMVQ: 36.610 → 36.536 tok/s | None |

## Latest ordinary-generation control

Prompt 16 baseline: 36.354623, 36.610121, 36.665497 tok/s; median
**36.610121 tok/s**. Candidate: four requests, median **36.536436 tok/s**
(−0.2013%). The candidate was rejected and production restored.
The fixture uses 1165 prompt / 511 completion tokens, seed 12345, thinking OFF.
The fixture is included at `prompt13_1/fixture.txt`.
This differs from the Prompt 12 qualification fixtures; compare matched A/B
results rather than treating different baseline medians as an optimization gain.

All seven Prompt 16 outputs matched SHA-256
`9b26a2ddd9518368939d4d29f107f398e793acd5f5cbdb926b2a9ffaa900c4f7`.
MTP drafted 391 / accepted 253; WDDM remained 86.628429 GiB.

## Measurement contract

Use **DEPENDENCY-BOUND GRAPH COMPLETION WALL TIME** for actual production
verification. HIP elapsed timestamps from production suffix graphs remain
untrustworthy. Synthetic event results do not validate production GPU timings.
Graph node counts establish structure, not per-kernel performance.

## Contents and provenance

Top-level summaries and JSON reports document each task. Subdirectories contain
selected raw A/B records, source/graph audits, microbenchmark evidence and focused
test logs. Trial patches are **rejected historical experiments**, provided for
reproducibility; they are not intended production patches. No trial binaries are
included. Original reports' LOCAL ONLY / remote-unmodified statements describe
those historical task executions; this later checkpoint was explicitly authorized.

Absolute user/workspace/model paths have been replaced with `<user-home>`,
`<workspace>`, `<model-store>` and `<pinned-gufo>`. Numerical results and output
hashes are preserved. `manifest.json` records both original and packaged hashes.

No model weights, dependencies, ROCm installation, binaries, build caches,
node modules, live configuration, environment files, Git credentials or agent
credential files are included. Current unrelated uncommitted source is not part
of this diagnostics checkpoint.

The next structural candidate is documented in `prompt16/next-candidate.json`;
it has not been implemented or benchmarked. No performance claim is made for it.
