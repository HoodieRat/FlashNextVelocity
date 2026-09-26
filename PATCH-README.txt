FlashNextVelocity Settings + Compact Zero-Draft Fix
2026-09-25

Apply:
  Extract this ZIP directly over the existing FlashNextVelocity project root.
  Replace files when prompted.
  Run BUILD.bat.

Fixes:
1. BUILD.bat now preserves the live dist\config.json and dist\ui.json before
   rebuilding dist, then restores them exactly. It no longer imports a config
   from C:\flashfknworkalready or another old project. First-build defaults now
   explicitly contain draft_confidence.

2. Settings Save is atomic and verified. Every visible engine/sampling/thinking
   setting is serialized, read back from the actual live dist\config.json, and
   compared before the UI continues.

3. Engine startup now verifies /health against the exact saved configuration.
   If the engine somehow starts with stale settings, startup fails with a field-
   specific mismatch instead of silently running the wrong values.

4. Benchmark saves current controls and restarts the engine only when the saved
   config differs from the engine configuration that was actually started.
   This prevents a benchmark from silently using old draft_max/draft_confidence.

5. Dashboard MTP status shows the active engine draft max and confidence.

6. Fixes: "decode failed: compact verification requires at least one draft row".
   With draft confidence enabled, the first proposed draft can legitimately be
   dropped, leaving an anchor-only target verification pass. That pass now uses
   the normal pinned full frontier row. The compact transfer also has a second
   defensive one-row fallback in Executor::Forward. No acceptance or sampling
   mathematics are changed.

Expected after rebuild:
- Set MTP draft max = 6 and confidence = 0.70.
- Click Save + Restart Engine (or Run Benchmark; benchmark will apply changed
  settings automatically).
- Dashboard should show: MTP ON · max 6 · conf 0.70
- Benchmark EFFECTIVE SETTINGS should show 6 and 0.70.
- A low-confidence stop on the first draft must no longer produce the compact
  verification error.
