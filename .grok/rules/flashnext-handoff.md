# FlashNextVelocity handoff rules
- Read GROK-HANDOFF.md before substantial work.
- Treat the extracted tree as project-owned source.
- Never make a durable fix only inside .deps/, build/, or generated dist files.
- Preserve exact target sampling and verification correctness.
- Use BUILD.bat for Windows/gfx1151 validation.
- Prefer measured benchmark evidence over speculative tuning.