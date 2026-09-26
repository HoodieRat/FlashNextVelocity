FlashNextVelocity Grok Handoff Tool V3

FIX
---
V3 is compatible with Windows PowerShell 5.1 / .NET Framework.
It does not use System.IO.Path.GetRelativePath().

PURPOSE
-------
Creates a clean, rebuildable Grok working copy of your CURRENT project state.

SAFETY
------
The live FlashNextVelocity project is READ ONLY to this tool.
It does not move, rename, modify, or delete anything in the project.
It only reads and copies files.

The only deletion is the tool's OWN temporary staging directory under %TEMP%
after the ZIP has been created.

HOW TO USE
----------
1. Put MAKE-GROK-HANDOFF-V3.ps1 and MAKE-GROK-HANDOFF-V3.bat in the root of
   your CURRENT FlashNextVelocity project.
2. Double-click MAKE-GROK-HANDOFF-V3.bat.
3. It creates ONE ZIP BESIDE the project folder.
4. Extract that ZIP into a NEW folder, for example:
      C:\FlashNextVelocity-Grok
5. Run INIT-GROK-WORKSPACE.bat once.
6. Run RUN-GROK.bat.

WHAT IT KEEPS
-------------
Everything project-owned by default, including source, scripts, overlays,
tests, documentation, configs, Serena project config/memories, Grok rules,
and unusual top-level project folders that were not specifically anticipated.

It also keeps a small number of the newest useful logs/benchmarks.

WHAT IT EXCLUDES
----------------
Generated/rebuildable/heavy content:
- .deps
- .cache
- build
- generated dist binaries
- downloaded archives
- model GGUF/safetensors
- native binaries
- old payload/backups
- Serena indexes/caches
- old log/benchmark history
- ordinary bin/obj/node_modules/etc.

BUILD.bat should recreate the excluded dependencies/build outputs.

CREDENTIAL SAFETY
-----------------
Files that appear to contain API keys, secrets, passwords, or tokens are
skipped with a warning instead of being copied into the handoff ZIP.
