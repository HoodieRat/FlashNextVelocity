FlashNextVelocity v1.0.8 TOP-WINS ROOT OVERLAY

Baseline: the uploaded v1.0.6 project, Gufo pin 9cad13974cf6da0cd3674b4e0a88b14b7e4a2908.

APPLY
1. Close FlashNextVelocity Studio / the engine.
2. Extract THIS ZIP directly over the project root.
3. Choose Replace files when Windows asks.
4. Run BUILD.bat.
5. Run Studio normally.

There is deliberately NO wrapper directory in this ZIP. README.md, src\..., scripts\..., and gufo-overlay\... are at the ZIP root.

TOP IMPLEMENTED WIN
- Compact sampled verification no longer downloads the final full-vocabulary target row every successful speculative round. The exact Top-256 certificate is used first; the authoritative full row remains on GPU and is downloaded only on exact-certificate fallback, rejection-frontier handling, snapshot save, or explicit Logits() access.

ALSO INCLUDED
- Preserves/build-gates the LLVM-23 gfx1151 W8A8 spill fix already present in the v1.0.6 baseline.
- Removes the live hard-coded dependency on C:\flashfknworkalready from the ROCm helper.
- Adds BENCHMARK-TOP-WINS.bat and a repeatable fixed-prompt/fixed-seed A/B harness.
- Adds a native telemetry regression test for deferred full-row D2H accounting.
- Bumps runtime/desktop version to 1.0.8 and runtime revision to fnv-top-wins-deferred-frontier-v10 so stale binaries are rejected.

NOT CLAIMED
No target-hardware tok/s result is invented here. The new A/B harness is included specifically to measure the real gfx1151 result after BUILD.bat.

See OPTIMIZATION-PASS-v1.0.8.md and VALIDATION-v1.0.8.txt.
