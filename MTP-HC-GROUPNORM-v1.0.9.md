# FlashNextVelocity v1.0.9 MTP HC-group normalization repair

## Finding

The current Qwen3.8 Flash-Next Gufo MTP path normalized the handed-over hidden tensor as one `HC * hidden` RMS group. The preserved Halo/Strix Qwen4Exp MTP graph instead reshapes the hidden state to `[hidden, hc, tokens]` and applies RMSNorm independently to each HC stream before applying the stream-specific `nextn_hnorm` gamma.

That distinction is mathematically significant. Whole-row normalization gives every stream the same RMS denominator; per-stream normalization gives each stream its own denominator. The next operation already projects the hidden tensor as `n_tokens * hc_count` independent rows through `nextn_fc_hidden`, so per-stream normalization is also consistent with the existing projection geometry.

## Patch

- `executor.cpp`: MTP hidden RMSNorm groups changed from `1` to `c.hc_count`.
- `batch.cpp`: same fix for multi-session batched MTP.
- `apply-gufo-overlay.ps1`: patches the pinned Gufo scalar reference oracle to normalize each HC stream independently.
- `build.ps1`: refuses to build unless all three paths contain the grouped normalization.
- Runtime and health/benchmark telemetry identify the repaired path as `per-HC-stream`.

## Why this is the next high-value test

Your controlled sweep held prompt and sampling settings constant while MTP acceptance stayed around 60-65% and additional draft depth rarely survived. That points at proposal correctness/quality rather than draft-max policy. A hidden-state normalization mismatch occurs before the MTP proposal head and therefore can depress the first proposal token as well as every later proposal.

This patch does not change target sampling, acceptance rules, RNG, confidence thresholds, QSA/PLE semantics, model weights, quantization, or v1.0.8's exact compact verification.

## Measurement goal

Re-run the same prompt/settings and compare MTP-only acceptance, first-position prefix survival, accepted/output token, decode tok/s, proposal ms/output, verification ms/output, and sync ms/output. The old runtime's 90%+ result remains a historical reference, not a promised outcome for this patch.
