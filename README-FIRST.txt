FlashNextVelocity v1.0.5 Strix Native Integration - DROP-IN PATCH
=================================================================

EASIEST INSTALL
1. Extract this ZIP anywhere.
2. Double-click APPLY-PATCH.bat.
3. If your project is not C:\FlashNextVelocity and the patch folder is not
   sitting inside the project folder, enter the project path when prompted.
4. The installer verifies the exact source bytes before changing anything,
   backs up every replaced file, applies the patch, then verifies every result.
5. Run BUILD.bat from the FlashNextVelocity project root.

BUILD.bat now compiles and runs native unit tests for the context-lookup and
proposal-source profiling logic before it builds the real Windows HIP engine.
The existing runtime self-test remains the final build gate when a model is
configured.

ROLLBACK
Double-click ROLLBACK-LAST-PATCH.bat. It restores the exact backed-up source
and removes files that this patch added. It refuses to erase post-patch edits
unless you explicitly force it.

FOR MODIFIED SOURCE TREES
APPLY-PATCH.bat intentionally stops if a source file differs from the supplied
v1.0.4 AI-context bundle. This prevents accidental clobbering. If you know the
local differences are safe, APPLY-PATCH-FORCE.bat still makes a full backup
before replacing them.

WHAT THIS PATCH ADDS
- Native Windows context/prompt lookup speculative lane beside MTP.
- Exact target verification for lookup proposals, including sampled p/q
  rejection + residual correction.
- MTP recurrent-state catch-up so lookup cycles cannot stale the draft block.
- Lookup outcomes isolated from the MTP draft-length learner.
- MTP-only vs lookup-only acceptance, prefix-survival and economics telemetry.
- Dedicated Studio controls and benchmark display for context lookup.
- Windows physical-RAM AND commit-headroom safety guard.
- Build-time source revision gates and native unit tests.

INTENTIONALLY NOT IMPORTED
- WSL2 / DXG / Linux mmap or /proc hooks.
- The published acceptance-EMA adaptive depth experiment (measured negative).
- The published 512-expert routing experiment (near-zero gain with HIP graphs).
- The experimental reduced-vocabulary MTP conversion path that faulted during
  expansion and is incompatible with the current shared Q8_0 sidecar format.

Runtime revision after patch: fnv-context-lookup-economics-v8
Project version after patch: 1.0.5
