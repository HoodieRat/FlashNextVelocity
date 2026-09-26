FlashNextVelocity stale-build/settings/compact-anchor repair
2026-09-25

WHAT THIS FIXES
1. Prevents CMake/Ninja from silently reusing old native object files after a ZIP/overlay source replacement.
2. Preserves and read-back verifies all Settings values in dist\config.json.
3. Preserves dist\config.json and dist\ui.json across BUILD.bat.
4. Removes the old anchor-only compact-verification fatal path.
5. Adds a runtime revision handshake between desktop and native engine so a stale engine cannot be accepted as healthy.

ROOT CAUSE OF THE REPEATED POPUP
The exact message:
  compact verification requires at least one draft row
is absent from the repaired source but existed in the previous executor.cpp.
`cmake --fresh` clears CMakeCache.txt but does not remove Ninja object files.
ZIP/overlay sources may retain timestamps older than existing .obj files, so Ninja can relink an old executor object even after the source was replaced.

BUILD BEHAVIOR AFTER THIS PATCH
- Gufo overlay is reapplied as before.
- Build validates that the repaired engine.cpp/executor.cpp source is present.
- The entire project build directory is deleted before configure/build.
- Native and desktop code are therefore rebuilt from current source.
- After the engine is built, BUILD checks the EXE:
    * old fatal string MUST be absent
    * runtime marker fnv-compact-anchor-v3 MUST be present
  The build fails rather than packaging a stale binary if either check fails.

SETTINGS
Save writes every Settings control to the live next-to-EXE dist\config.json,
atomically, then reloads the file and verifies every field.
Save + Restart additionally launches the engine from that exact config.
Engine startup verifies /health against all saved engine/sampling/thinking fields.
Benchmark saves first and restarts automatically if active settings differ.

COMPACT ZERO-DRAFT CASE
When draft confidence rejects the first proposal, the verification batch contains
only the anchor/frontier row. This is valid. The compact path now degrades to the
existing full pinned frontier transfer for that cycle. It is not an error and does
not alter MTP acceptance math.

RUNTIME REVISION
/health now exposes:
  runtime_revision: fnv-compact-anchor-v3
The desktop requires that exact revision before considering the engine started.
This catches wrong/stale engine binaries or an app launched from an older dist.

INSTALL
1. Fully exit FlashNextVelocity/tray app.
2. Extract this ZIP over the existing project root and replace files.
3. Run BUILD.bat. This build intentionally recompiles the native engine and desktop.
4. Look for:
   Native engine source revision verified: fnv-compact-anchor-v3; legacy compact fatal absent.
5. Launch dist\FlashNextVelocity.exe.
6. Set Draft max=6 and Draft confidence=0.70.
7. Click Save + Restart Engine.
8. The log should say all settings were saved/read-back verified, then active config verified.
9. Run one benchmark.

DO NOT merge this into a different numbered project tree. Apply it to the current project root.
