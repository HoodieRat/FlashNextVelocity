# Benchmarking FlashNextVelocity

A compact, reproducible API benchmark with a terminal summary, a GitHub Markdown report, two SVG charts, and raw JSON/CSV results. It uses Python's standard library; no benchmark packages are required.

## Quick start

1. Build the project and configure the target model and MTP sidecar in Studio.
2. Stop the engine in Studio. The suite needs the GPU memory for its own launches.
3. Double-click **`BENCHMARK-REPORT.bat`** from the project folder.

The runner loads one engine at a time, with temporary configuration copies under `.cache/benchmark-report/`. It never rewrites `dist/config.json`, kills an existing engine, or publishes anything to GitHub. Its own engine processes are stopped at completion or cancellation. Windows job objects also stop them if the benchmark terminal is closed.

The default time limit is **45 minutes**, including asset hashing and model loading. The expected duration is approximately **20–40 minutes**, but this depends on hardware, prompt processing, and the saved inference settings. An independent timer terminates the owned engine at the deadline. Cleanup/report writing can take a few more seconds. Incomplete or failed conditions remain visible in the report.

Afterward, start your normal engine through Studio when needed.

## Outputs

| File | Purpose |
| --- | --- |
| `BENCHMARK-REPORT.md` | Latest GitHub-ready report at the repository root |
| `benchmark-reports/<UTC timestamp>/README.md` | Self-contained report for that run |
| `workloads.svg`, `context.svg` | Relative image links that render on GitHub |
| `raw-results.json` | All requests, outputs, effective settings, metrics, identities, and errors |
| `requests.csv` | One row per measured, warmup, or setup request |
| `inputs.json` | Exact prompt snapshots for replay on another setup |

Commit the root report **and its corresponding run folder** so the relative links work. Alternatively, publish just the self-contained run folder. Generated reports are outside the ignored `benchmarks/` directory.

## Suite and request count

| Group | Conditions | Measured requests |
| --- | ---: | ---: |
| Prose, code generation, code editing × three acceleration modes | 9 | 27 |
| Approximately 8k, 32k, 64k occupied context | 3 | 9 |
| Compact JSON and a simulated tool call | 2 | 6 |
| Conversation continuation with a matched fresh control | 1 | 6 |
| Longer generation | 1 | 1 |
| **Total** | **16** | **49** |

There are also **six warmups**, **three conversation priming requests**, and **three to nine context calibration requests**. Thus the default completed run makes **61–67 total HTTP generation requests**. The short setup calls are excluded from performance medians. The three extra measured requests relative to the original 46-request proposal provide a fresh control for conversation continuation.

Typical outputs are capped at 256 tokens, compact JSON/tools at 128, and longer generation at 1,024. Early EOS is retained and explicitly reported. The runner does not force output after EOS, execute tools, or run generated code.

## Options

Run these commands from the project folder:

```bat
REM Default: production sampling, three repetitions, 45-minute cap
BENCHMARK-REPORT.bat

REM Greedy preset for a separate, clearly labeled comparison
BENCHMARK-REPORT.bat --preset greedy

REM Shorter run; report is explicitly marked partial if the limit is reached
BENCHMARK-REPORT.bat --budget-minutes 15 --repetitions 1

REM Inspect report layout and planned conditions without loading the model
BENCHMARK-REPORT.bat --plan

REM Compare with a previous report; mismatched conditions receive no delta
BENCHMARK-REPORT.bat --compare benchmark-reports\PREVIOUS-RUN\raw-results.json

REM Recover/re-render a saved run without making any inference requests
BENCHMARK-REPORT.bat --render benchmark-reports\RUN\raw-results.json

REM Correct the integer-quantity edit checks using saved responses, without inference
BENCHMARK-REPORT.bat --recheck-edits benchmark-reports\RUN\raw-results.json
```

`--port` changes the private benchmark endpoint (default `18080`). `--request-timeout` and `--startup-timeout` default to 180 seconds. `--output` changes the archive directory. Python 3.10 or newer is required; the launcher prefers `py -3` and falls back to `python`.

`--plan` replaces the root latest report with a clearly labeled plan; previous run folders are retained. Use `--render` to restore a previous run as the latest report.

`--recheck-edits` writes a separate annotated `-rechecked` run folder and updates the root report. It preserves the original archive, outputs, settings, timings, and elapsed benchmark duration. The raw results record every previous/new check and the source file hash. Rechecking refuses a different saved edit prompt. It does not fill unmeasured conditions or load the engine.

## Measurement contract

### Configuration

The saved Studio model, MTP, proposal policy, confidence, batch size, lookup policy, memory guard, and sampler are the inputs. The runner changes only the benchmark copy:

- Loopback host and a private port.
- Identical context capacity across modes, at least 67,584 tokens for the 64k case.
- One session, thinking off, vision off, and no private Studio agent prompt.
- A published system prompt and a fixed three-seed schedule.
- No MTP sidecar in the speculation-off load; no lookup in the MTP-only load.
- Profiling off for every measured request.

The report records these overrides. The `production` preset means production **sampling**, not an assertion that every runtime setting equals a previous qualification report. A `greedy` run sets temperature to zero, disables sampler truncation/penalties, and remains a separate result set. Mode order is grouped to require only three model loads; it does not supply interleaved A/B evidence.

### Cache and context

The native recurrent session restarts whenever the prompt diverges from its stored token sequence. Fresh chat requests, including repetitions after generation, use this path. The context corpus is a snapshot of actual repository documents/code. Calibration aims within 2% of the target depth and always records the actual count. Source changes alter prompt fingerprints; comparisons must use the same `inputs.json` snapshots.

Continuation uses `/v1/completions`: a short generated prefix is appended to the exact raw input, then continued. The same input is replayed after that generation as a fresh control. Re-tokenization and EOS can prevent reuse. The engine does not expose cached-token counts, so the report labels this as a **continuation attempt** and does not invent a cached-token count or cached prefill tps. This condition uses buffered completion responses; client first-response/gap metrics are unavailable for it.

The continuation fixture now includes Qwen's non-thinking chat framing explicitly, because the raw completion endpoint does not add it. The original unframed prime returned an immediate EOS with zero output tokens; its zero decode rate was valid, and continuation had not started. Failed measurements now retain response text, finish reason, token counts, and metrics; immediate EOS is labeled as unmeasurable throughput. Historical partial reports are preserved and are not retroactively completed by this fix.

### Timing and correctness

- Engine decode tps divides final generated tokens by engine decode wall time.
- Client first response is timed from before request submission to the first meaningful content/reasoning/tool SSE payload. Role/heartbeat/usage messages are excluded.
- Streaming gap statistics describe payload events, which can contain multiple tokens. They are not precise per-token execution times.
- Client total time includes tokenization, request handling, sampling, streaming, and final response delivery.
- Fresh prefill tps uses the engine metric. Continuation uses prefill milliseconds because actual new-token counts are unavailable.
- Medians/ranges come from repetitions. No p95/p99 request latency is claimed from three repetitions.
- JSON/tool checks verify exact values. Code editing compares syntax trees against the expected edit. Code generation checks parsing and function presence, without executing it. Prose checks only basic response integrity.

Edit checker revision 2 accepts summing integer quantities directly or with optional `int()` conversion, while requiring all unrelated code to retain its AST. Revision 1 incorrectly required conversion even though the prompt did not request it. The prompt and generation settings are unchanged, so existing edit responses can be rechecked offline.

Successful timing and task correctness are reported separately. A truncated or incorrect output can have a measured speed and a failed task check. A complete report with failed checks returns exit code `2`, as do partial/interrupted runs. Startup errors return `1`; a complete passing report returns `0`.

## Scope and comparison

This is a single-client **API workload suite**, not a `llama-bench` microbenchmark. Compare it against another engine using the same prompts, model/sidecar, sampling, occupied context, output caps, and cache conditions. A fixed seed does not guarantee byte-identical output across different backends.

Three repetitions are suitable for initial screening. Small gains need focused interleaved validation. Memory is sampled at request boundaries; it is not a guaranteed peak. No temperature/clock/power trace, physical disk-throughput trace, pure GPU graph timing, concurrency, vision, reasoning-on, broad quality evaluation, or overnight stability result is claimed.

Local engine logs and launch configurations stay under ignored `.cache/`. Public report files use model filenames rather than private model-directory paths. Check generated outputs before publishing, as with any report containing model responses.

## Focused lookup policy comparison

```bat
python scripts\benchmark_lookup_policy.py
```

This compares `fixed6` with `sticky` using one engine load at the current production context and sampler. It makes **18 measured requests plus six full warmups**, alternating the policy order in each pair. The default time limit is **20 minutes**, including asset hashing and loading.

The workloads are a complete edit of a repository Python module, exact quotation of twelve log rows, and a prose control. Outputs, source snapshots, expected answers, actual token counts, sampler checks, promotion telemetry, and paired hashes are retained. The file-edit check compares the entire AST; generated code is not executed.

Results appear in the terminal, `LOOKUP-POLICY-REPORT.md`, and a timestamped `benchmark-reports/*-lookup-policy/` folder. Raw JSON and inputs are saved after each request. The full-suite report is kept separately. The runner uses a temporary configuration and owns its engine; it does not change production settings or stop another engine.

Qualification gates are defined before measurement: all 18 measured checks pass; copy-workload hashes match; at least one copy workload gains 5% with all three pairs faster; no workload median regresses by more than 3%; and a copy workload promotes in all three sticky repetitions. Passing is evidence for these workloads and settings, not a broad quality or stability certification.
## Default Windows profile

The October 2, 2026 focused run qualified sticky lookup for the current Windows profile: 32,768-token context, MTP draft limit 7, confidence 0.75, distribution proposals with the Latin draft vocabulary, prefill batch 4,096, and lookup n-grams 5–7 over a 32,768-token window. Sticky starts at 6 tokens and promotes to 16 after two qualifying lookup rounds; capacity is 16. Sampling is temperature 0.35, top-p 0.9, top-k 20, min-p 0, and repeat penalty 1.

These are the fresh-install and Studio Restore Default performance settings. Existing configurations are preserved across builds. The live Studio profile was switched from fixed6 to sticky after qualification. See [the focused report](../LOOKUP-POLICY-REPORT.md): file editing improved 16.3%, log quotation 10.0%, and copied outputs matched exactly. Prose speed was essentially unchanged; broad answer quality was not evaluated. Memory guard and output limits remain independently configurable.
