FlashNextVelocity v1.0.6 - TRUE DROP-IN OVERLAY

INSTALL
1. Stop FlashNextVelocity / the engine.
2. Extract THIS ZIP directly into the ROOT of your existing FlashNextVelocity project.
3. Choose Replace files in the destination.
4. Run BUILD.bat.
5. Start the Studio. The new Settings fields are:
   - MTP proposal mode: halo_greedy (new default) or distribution (A/B fallback)
   - Prefill batch (real Gufo): 2048 default

IMPORTANT FIRST TEST
For a clean old-vs-current MTP acceptance test:
- MTP proposal mode = halo_greedy
- MTP draft confidence = 0.00
- Context lookup = OFF
- keep temperature/top-p/top-k/min-p/repeat penalty exactly the same
Run the same benchmark/prompt you used before. Then switch only MTP proposal mode to distribution and repeat.

WHY
The preserved old halo-box runtime drafted the MTP head's top token and let the target sample decide acceptance. Current Gufo sampled a separate Top-256 proposal q and used p/q rejection. v1.0.6 restores the old proposal semantics as an explicit exact mode while keeping Gufo distribution mode available.

The build is revision-gated. BUILD.bat refuses stale source/runtime combinations.
See OPTIMIZATION-PASS-v1.0.6.md for details.
