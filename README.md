# FlashNextVelocity Studio 1.0.9

A Windows desktop/tray application and native `gfx1151` inference server for Qwen3.8-Flash-Next on AMD Strix Halo.

## Current Windows defaults and measured results

Fresh installs and Studio **Restore Default** use the measured 32k context profile: distribution MTP with the shared Q8_0 sidecar and Latin vocabulary, draft limit 7, confidence 0.75, prefill batch 4,096, and sticky context lookup. Lookup starts at 6 tokens and can promote to 16 within a request. Sampling is temperature 0.35, top-p 0.9, top-k 20, min-p 0, and repeat penalty 1. Existing settings are preserved across builds.

The [focused lookup comparison](LOOKUP-POLICY-REPORT.md) measured **16.3% faster file editing** and **10.0% faster log quotation** against fixed6, with identical copied outputs and all 18 measured checks passing. Prose throughput was essentially unchanged. These results apply to the published workloads; broad answer quality was not evaluated.

The [API benchmark report](BENCHMARK-REPORT.md) includes throughput, latency, context depth, task checks, and reproducibility artifacts. That run is partial: 43 completed checks passed, but the conversation condition failed. See the [benchmark guide](docs/BENCHMARKING.md) for commands and measurement limits.

This package is a **clean project**, not a hotfix overlay. It does not contain the old numbered repair BAT files.

## What you get

- `FlashNextVelocity.exe`: Windows tray/dashboard application.
- `FlashNextVelocity.Engine.exe`: native Gufo-derived HIP inference engine, managed by the desktop app.
- OpenAI-compatible API at `http://127.0.0.1:8080/v1`.
- Browser dashboard at `http://127.0.0.1:8080/` with quick chat and benchmark controls.
- Desktop settings for model, MTP, native context lookup, Windows memory guard, vision, context, draft depth, sampling and server port.
- Desktop benchmark page with prefill tok/s, decode tok/s, MTP-only acceptance, lookup acceptance, per-position survival/economics and 64-token decode windows.
- Logs page and persistent engine logs.
- System-tray Start / Stop / Restart / Open Dashboard controls.
- Optional Start with Windows.
- F16 or BF16 Qwen3.8 Flash-Next vision sidecar support.
- Strict build-time runtime validation: health, API discovery, browser dashboard, text/code generation, real vision inference (when configured), and benchmark/metrics.

## Install/build

Put the project at a short Windows path, preferably:

`C:\FlashNextVelocity`

Then double-click:

`BUILD.bat`

The build is project-local. It installs/builds the required native dependencies, pins the HIP host toolset to MSVC 14.44, uses the pinned Gufo commit, builds the native engine, builds the Windows tray app, creates `dist\`, then starts the real engine for runtime self-tests.

On the first v1.0.4 run/build, Studio can copy an existing ROCm gfx1151 SDK from the old project into its own `.deps\rocm`. After that, Studio no longer depends on `C:\flashfknworkalready` and that old folder may be deleted.

If `C:\flashfknworkalready\config.json` exists, it is migrated into the new app for the first build. Otherwise the desktop app searches `C:\FlashNextModels` for the model, shared Q8_0 MTP and mmproj on first launch.

A build is considered successful only after the full runtime acceptance gate passes when a model is configured. The gate verifies the dashboard/API, real text generation, real vision inference with a CRC-valid PNG fixture, and benchmark metrics. Compilation alone is not treated as success.

Repeated builds reuse the already-prepared pinned Gufo tree unless the pinned commit or integrated Windows preparation script changes, avoiding unnecessary native recompilation.

## Normal use

After a successful build, double-click:

`RUN.bat`

From then on you normally use the tray icon or dashboard, not scripts.

Closing the dashboard window minimizes it to the tray. Use the tray menu **Exit** to stop the native engine and leave the app.

## Dashboard

The desktop dashboard has five tabs:

- **Dashboard**: engine status, MTP/vision state, API URL and a real chat/code test.
- **Settings**: model, MTP, mmproj, context, MTP draft max, host/port and sampling controls. `Save + Restart Engine` applies engine-level settings.
- **Agent Prompt**: editable system instructions, prefilled with a coding and SVG design prompt. `Save + Apply` saves it for new Quick Chat, browser chat, and API requests that explicitly supply it without reloading the model. An empty prompt disables these additional instructions; client system instructions and tools are retained.
- **Benchmark**: warmup plus measured inference, including prefill tok/s, decode tok/s, MTP acceptance and successive ~64-token decode windows.
- **Logs**: live native stdout/stderr and access to persisted logs.

Benchmark reporting requirement: every Settings-page value must appear in the displayed benchmark report and saved JSON, including disabled settings, model paths, lookup parameters, memory guard, sampling, reasoning preferences, active config path, and Studio startup preferences. Studio captures this snapshot before running; request overrides and their effective values are reported separately. Lookup telemetry must identify `sticky`, `fixed6`, or `fixed16`, its starting/maximum/final widths, capacity, and whether/where promotion happened. Last Request includes the engine configuration captured for that request; local Studio preferences are labeled with their refresh time context.

Quick Chat is a standalone chat: code and SVG requests return code in the response. It has no filesystem or shell tools. Coding clients such as OpenCode supply their own tools and execute the structured calls returned by the server.

The Studio agent prompt is stored as `agent_prompt` in `dist/config.json`. Desktop and browser Studio explicitly send it with their requests. External chat API requests add no Studio prompt when `agent_prompt` is omitted, null, or empty. An explicit string is added once before client messages; other non-null types return HTTP 400. `/health` exposes the live saved prompt as `studio_agent_prompt` for browser Studio. Model and sampling settings still affect output quality.

Studio launches the engine with an immutable configuration snapshot and the internal `--studio-config` argument pointing to the durable editable configuration. Prompt-only Save + Apply changes affect new Studio requests without reloading the model. Start/restart operations are serialized; Stop and Exit cancel initialization, and only the owned engine process is terminated. An occupied port produces a startup error.

The Settings page provides opt-in **Apply Thinking - Medium**, **Apply Thinking - Xhigh**, and **Apply Non-thinking** presets using Qwen's recommended sampling values. These fill controls without saving or restarting and preserve context, MTP, lookup, memory guard, seed, repeat window, paths, and token limits. Use Save + Restart to apply them. Thinking presets preserve supplied reasoning history; external clients must retain and resend `reasoning_content` to benefit from that setting.

Runtime `fnv-mtp-cache-replay-v13` selectively backports Gufo's MTP cache/replay correction: residual validity, cache-only prefill, consistent persistent projections, and corrected snapshot state. Older snapshots are rejected. Grouped HC normalization, asynchronous halo chains, exact target verification, and Windows I/O remain in place.

The measured Windows distribution/Q8/Latin/C1 configuration uses a separate calibrated cost curve. Its model, sidecar layout, serving geometry, and compact verification lane are checked before selection; other configurations retain the incumbent curve. Health and request metrics identify the selected profile. Adaptive depth and the draft maximum of 7 are retained.

The inference update can be staged with `scripts/build-inference-candidate.ps1` and installed with Studio closed using `scripts/install-inference-candidate.ps1`. Installation verifies binary hashes, preserves `config.json`, `ui.json`, and `runtime.json`, and retains the prior binaries under `build/inference-install-backup-*`.

Chat streams send text and reasoning as they are generated. Tool calls are sent only after the entire call has been parsed, with required arguments and declared basic types checked; interrupted calls never expose an empty placeholder to clients. Cancelling a client request releases the generation session. Text responses report `finish_reason: "length"` when the output budget runs out. A tool call cut off by the output budget returns an explicit error; streaming errors use the SSE error envelope.

For a real-model regression check with Studio already running, run `python tests/chat_integration.py`. It checks the pelican SVG prompt, tool-call/result continuation, streaming parity, output limits, reasoning, and disconnect cancellation. Outputs are saved under `benchmarks/chat-integration`.

Run `python tests/tool_stream_regression.py` for the shorter real-model regression covering complete writes, truncated writes, streaming/JSON error parity, and disconnect cancellation. Outputs are saved under `benchmarks/tool-stream-regression`.

There is also a browser dashboard served by the native engine:

`http://127.0.0.1:8080/`

Going to `http://127.0.0.1:8080/v1` now returns API information instead of a blank-looking endpoint.

## Use it from another project

Use this as the OpenAI base URL:

`http://127.0.0.1:8080/v1`

An API key is not required for the default loopback-only setup. Clients that require a non-empty key can use any placeholder value.

Python/OpenAI example:

```python
from openai import OpenAI

client = OpenAI(base_url="http://127.0.0.1:8080/v1", api_key="local")
response = client.chat.completions.create(
    model="local",
    messages=[{"role": "user", "content": "Write a Python quicksort."}],
    max_tokens=512,
)
print(response.choices[0].message.content)
```

Supported server routes:

- `GET /`
- `GET /dashboard`
- `GET /health`
- `GET /metrics`
- `GET /v1`
- `GET /v1/models`
- `POST /v1/chat/completions`
- `POST /v1/completions`

Chat supports ordinary text, streaming SSE, Qwen reasoning controls, tool schemas/calls, and OpenAI-style `image_url` content when a vision sidecar is configured.

## Runtime fixes integrated into the source build

There is no numbered patch chain. `BUILD.bat` checks out the exact pinned Gufo source and runs one deterministic source-preparation step containing the Windows platform integration:

- Windows GGUF memory mapping.
- Windows direct/overlapped PLE and weight-loading I/O.
- Winsock image networking.
- Windows plan-database locking/publication.
- removal of Linux-only THP calls.
- Windows LLP64 JSON overload fix.
- hipBLAS fallback for prompt projection shapes for which Windows hipBLASLt has no valid zero-workspace plan.
- complete F16 mmproj conversion into Gufo's BF16/F32 working representation during lazy vision upload, including matrix weights, patch embeddings, norms, biases and positional tensors.

The upstream BF16 mmproj path remains native. F16 is enabled when explicitly configured; a blank `mmproj` remains genuinely vision-off.

## Files that matter after build

Runtime files are under `dist\`:

- `dist\FlashNextVelocity.exe`
- `dist\config.json`
- `dist\runtime.json`
- `dist\engine\FlashNextVelocity.Engine.exe`
- `dist\logs\`
- `dist\benchmarks\`

The source/dependency/build trees can remain in place because the local ROCm runtime and model engine use them for runtime library locations.

## Licensing

Gufo is MIT licensed and pinned to the commit listed in `VERSION.json`. See `THIRD_PARTY.md` and `LICENSE`.



## v1.0.5 native speculative integration

This release adds a Windows-native prompt/context lookup proposal lane beside MTP. When the newly sampled target token completes a strong n-gram already present in committed context, Studio copies the historical continuation as a speculative proposal and verifies it through the existing Gufo target/rollback path. The MTP recurrent block still receives a headless catch-up so its state remains valid, but its proposal/output-head work is skipped for that lookup-selected cycle. If no qualifying match exists, generation falls through to the normal MTP path. Lookup width is independent of the MTP length learner because the two proposal sources have different costs.

Sampled decoding remains distribution-correct: each lookup token is represented as a deterministic one-hot proposal distribution and is passed through the same target p/q rejection plus residual-correction verifier used by sampled MTP. Lookup outcomes do not train the MTP draft-length policy. Metrics separately report MTP and lookup drafted/accepted counts and prefix survival.

A configurable Windows memory floor is also checked before/after model load, at request start, and periodically during generation. The guard checks both available physical RAM and commit headroom; the default floor is 8 GiB. It is a safety guard only; it does not change VGM or expose additional VRAM.

The integration is fully native Windows. No WSL2, DXG bridge, Linux `/proc`/`mmap` hooks, acceptance-EMA draft controller, experimental reduced-vocabulary MTP conversion, or no-gain 512-expert routing patch is imported.

## v1.0.4 ROCm ownership / hipBLASLt fix

Studio now owns `.deps\rocm` inside the project. `RUN.bat` performs a one-time migration from an older ROCm install when needed. The hipBLASLt environment is pointed at the architecture-specific `bin\hipblaslt\library\gfx1151` directory, where `TensileLibrary_lazy_gfx1151.*` and gfx1151 code objects live.

## v1.0.6: sampled-MTP recovery mode

`mtp_proposal_mode` is now explicit. `halo_greedy` restores the deterministic MTP-top-token proposal semantics used by the preserved high-acceptance halo-box runtime while keeping target sampling exact; `distribution` retains Gufo's stochastic Top-256 p/q proposal path for A/B testing. `prefill_batch` controls the real Gufo native prefill `max_batch` (default 2048), not a fake llama.cpp-style ubatch. See `OPTIMIZATION-PASS-v1.0.6.md` for the source-level finding and recommended comparison.

## v1.0.8: adaptive exact target-verification shortlist

The sampled target verifier now selects the smallest exact raw shortlist that safely contains the request's top-k support: Top-64 for the default `top_k=40`, Top-128 for `64..127`, Top-256 for `128..255`, and the existing full-vocabulary path when compact verification cannot be certified. The strict support certificate, exact target RNG stream, rollback behavior, and synchronized full final frontier are preserved.

Studio-generated chat and benchmark responses are also checked against the engine's request-effective settings. If thinking, reasoning effort, sampling, draft, proposal, prefill, context-lookup, or context values do not match what the UI sent, Studio raises a mismatch instead of silently presenting a setting that was not actually applied.

See `OPTIMIZATION-PASS-v1.0.8.md` and `VALIDATION-v1.0.8.txt`.

## v1.0.9: restore trained MTP hyper-connection normalization

The Qwen3.8 Flash-Next MTP hidden handoff is now RMS-normalized independently per hyper-connection stream, matching the preserved high-acceptance Halo/Strix implementation and the head's row-wise hidden projection. The previous Gufo path normalized the entire HC*hidden row with one shared RMS scale, coupling streams before `nextn_fc_hidden` and changing the MTP proposal distribution.

The fix is applied to both single-session and batched ROCm MTP execution. The build overlay also patches Gufo's optional scalar reference oracle to the same geometry, and `BUILD.bat` rejects a source tree where any of those paths regress to whole-row normalization. v1.0.8's adaptive Top-64/128/256 exact target verification remains intact.

Benchmark telemetry now reports `MTP hidden normalization: per-HC-stream`.

See `MTP-HC-GROUPNORM-v1.0.9.md` and `VALIDATION-v1.0.9.txt`.

## GitHub benchmark report

Run **`BENCHMARK-REPORT.bat`** for a bounded text benchmark: 16 conditions covering prose, code generation, code editing, acceleration modes, occupied context, JSON/tools, conversation continuation, and longer generation. Stop the normal engine in Studio first; the runner uses three sequential launches with temporary configuration copies.

The default 45-minute limit includes loading and model fingerprinting. Results appear in the terminal and in [BENCHMARK-REPORT.md](BENCHMARK-REPORT.md), with charts, input snapshots, JSON, and CSV in the corresponding `benchmark-reports/` run folder. Commit both the report and its linked run folder to display everything on GitHub.

See [Benchmarking guide](docs/BENCHMARKING.md) for methodology, options, comparison rules, and report recovery. `BENCHMARK-REPORT.bat --plan` previews the report without loading the model.
