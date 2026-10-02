# Lookup Policy Qualification

> **COMPLETE** · 2026-10-02T02:49:32.089730+00:00 · One engine load · Interleaved policies

## Summary

**Decision: PASS.**

| Workload | Fixed6 tps | Sticky tps | Change | Checks fixed6 / sticky | Sticky promotions |
| --- | --- | --- | --- | --- | --- |
| file_edit | 93.8 | 109.1 | 16.3% | 3/3 · 3/3 | 3/3 |
| log_quotation | 78.2 | 86.0 | 10.0% | 3/3 · 3/3 | 3/3 |
| prose_control | 36.7 | 37.0 | 0.7% | 3/3 · 3/3 | 0/3 |

The decision gates were defined before measurement: all 18 checks pass; copied-output hashes match; at least one copy workload gains 5% with all three pairs faster; no workload median loses more than 3%; a copy workload promotes in all three sticky repetitions.

| Gate | Result |
| --- | --- |
| all 18 measured checks pass | PASS |
| copy output hashes match | PASS |
| at least 5 percent copy gain and all three pairs faster | PASS |
| no workload median regression over 3 percent | PASS |
| copy workload promotes in all three sticky runs | PASS |

## Method

- Three workloads × two policies × three repetitions = 18 measured requests, plus six full warmups.
- Fixed6/sticky order alternates across pairs; the paired requests share the same prompt and seed.
- Current production context, sampler, MTP policy, confidence, batch size, n-gram range, and search window. Benchmark copies use one session, text only, thinking off, and the published system prompt.
- Common lookup capacity 16 and configured maximum 16. Only the request policy switches; fixed6 stays at six tokens, sticky can promote to sixteen. The model is not reloaded between policies.
- Fresh prompts reset the recurrent session after the preceding generation. All outputs and actual token counts are saved.
- File edit uses an actual repository Python module and checks the complete AST. Log quotation checks all twelve rows exactly. Prose uses a basic integrity check. Generated code is not executed.
- Profiling is off. TPS is engine decode wall time; client total and first response have separate boundaries.

## Latency and Lookup

| Workload / policy | Input / output tokens | Client total s | First response ms | Lookup acceptance | Lookup accepted / output |
| --- | --- | --- | --- | --- | --- |
| file_edit / fixed6 | [958] / [866] | 10.1 | 958.3 | 99.0% | 0.837 |
| file_edit / sticky | [958] / [866] | 8.8 | 944.3 | 95.9% | 0.917 |
| log_quotation / fixed6 | [716] / [635] | 8.8 | 801.1 | 77.2% | 0.817 |
| log_quotation / sticky | [716] / [635] | 8.1 | 802.1 | 72.4% | 0.910 |
| prose_control / fixed6 | [101] / [256] | 7.2 | 307.9 | N/A | 0.000 |
| prose_control / sticky | [101] / [256] | 7.2 | 315.0 | N/A | 0.000 |

## Paired Results

| Workload | Seed repetition | Same output hash | Sticky TPS change |
| --- | --- | --- | --- |
| file_edit | 1 | True | 15.9% |
| file_edit | 2 | True | 19.6% |
| file_edit | 3 | True | 16.1% |
| log_quotation | 1 | True | 12.2% |
| log_quotation | 2 | True | 10.0% |
| log_quotation | 3 | True | 11.4% |
| prose_control | 1 | True | 0.7% |
| prose_control | 2 | True | -0.1% |
| prose_control | 3 | True | 1.1% |

Sampling can produce different valid text across proposal policies. Copy-workload hashes are required for this qualification; prose is checked for basic integrity. No claim of universal seeded identity is made.

## System and Reproducibility

| Setting | Value |
| --- | --- |
| CPU | AMD RYZEN AI MAX+ 395 w/ Radeon 8060S           |
| Context | 32768 |
| Sampler | {"temperature": 0.35, "top_p": 0.9, "top_k": 20, "min_p": 0, "repeat_penalty": 1, "repeat_last_n": 512, "frequency_penalty": 0, "presence_penalty": 0} |
| Engine revision | fnv-mtp-cache-replay-v13 |
| Time limit | 20 minutes |
| Elapsed | 5.2 minutes |
| Runner SHA256 | 958ad8a5fa5ee4616bbf1d20234cd008391108f2036e113f80e8319cdc4923ed |

- [Raw results, outputs, metrics, hashes, settings, and memory samples](benchmark-reports/20261002-024932Z-lookup-policy/raw-results.json)
- [Exact prompts, original source, and expected answers](benchmark-reports/20261002-024932Z-lookup-policy/inputs.json)
- [Request CSV](benchmark-reports/20261002-024932Z-lookup-policy/requests.csv)

- [Runner source used for these measurements](benchmark-reports/20261002-024932Z-lookup-policy/runner-source.py)
- [Shared measurement helper source](benchmark-reports/20261002-024932Z-lookup-policy/helper-source.py)

## Limitations and Run Notes

- Three repetitions are a focused screening result. They do not establish tail latency, broad code quality, performance at other contexts, or memory peaks. No production settings are changed by this trial.
- Both policies reserve the same lookup capacity. Memory is sampled at request boundaries; these observations cannot establish the cost of lookup versus lookup disabled.
- Prose gains should not be inferred from workloads with no copied tokens. Search time and rejected-round cost are not separately instrumented in this unprofiled comparison.


<details>
<summary>Common benchmark configuration</summary>

```json
{
  "model": "Qwen3.8-Flash-Next-UD-Q4_K_XL-00001-of-00004.gguf",
  "mtp": "mtp-Qwen3.8-Flash-Next-shared-Q8_0.gguf",
  "mmproj": "",
  "host": "127.0.0.1",
  "port": 18081,
  "context": 32768,
  "draft_max": 7,
  "draft_confidence": 0.75,
  "mtp_draft_vocabulary": "latin",
  "mtp_proposal_mode": "distribution",
  "prefill_batch": 4096,
  "context_lookup": true,
  "context_lookup_min_ngram": 5,
  "context_lookup_max_ngram": 7,
  "context_lookup_window": 32768,
  "context_lookup_min_draft": 6,
  "context_lookup_max_draft": 16,
  "context_lookup_policy": "fixed6",
  "context_lookup_capacity": 16,
  "memory_guard": true,
  "memory_guard_min_available_gib": 3,
  "sessions": 1,
  "default_max_tokens": 32768,
  "thinking": false,
  "preserve_thinking": false,
  "reasoning_effort": "medium",
  "sampling": {
    "temperature": 0.35,
    "top_p": 0.9,
    "top_k": 20,
    "min_p": 0,
    "repeat_penalty": 1,
    "repeat_last_n": 512,
    "frequency_penalty": 0,
    "presence_penalty": 0,
    "seed": 12345
  }
}
```

</details>
