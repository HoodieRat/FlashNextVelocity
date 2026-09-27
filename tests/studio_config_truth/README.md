# Focused Studio configuration checks

From the repository root on Windows:

```powershell
dotnet run --project tests/studio_config_truth/StudioConfigTruth.csproj -- dist/config.json benchmarks/prompt11-tests
```

This instantiates the actual WinForms controls without displaying a window or
starting the engine. It checks production values, thinking OFF/ON with a remembered
effort, explicit zero/false, numeric precision, persistence, request payloads, and
independent lookup/MTP widths. Scratch settings stay in the test output directories.
The live production configuration is read only.

For native changes, build through `scripts/build.ps1 -SkipBundledTests`, then run
`python tests/studio_runtime_truth.py`. That integration check loads the configured
production model once, sends three non-streaming 8-token configuration requests and
one 64-token production stream, verifies effective values and unchanged model/process
identities, and stops its engine. It requires the local production model and ROCm
runtime. It does not measure or compare performance.

Verify the captured runtime health through Studio's actual startup check:

```powershell
dotnet run --project tests/studio_config_truth/StudioConfigTruth.csproj -- dist/config.json benchmarks/prompt11-tests benchmarks/prompt11-tests/health.json
```

Runtime sampler fields use native float precision; comparisons allow `1e-6` while
zero and boolean checks remain exact. Promotion during the short stream is optional;
every request must start at 6 with sticky capacity/promotion width 16 and MTP depth 6.
