FlashNextVelocity v7 performance patch

Purpose
-------
Restore the last validated controller behavior and make two exact sampled-MTP
optimizations without changing target sampling, acceptance math, residual
correction, model weights, QSA, PLE, or batch semantics.

Changes
-------
1. Removes the experimental FNV Windows C1 shallow cost override and restores
   Gufo's calibrated cost curves. The v6 override did not improve the measured
   Windows run and shifted the controller away from the prior 32.7 tok/s baseline.
2. Expands sampled MTP raw draft support from Top-64 to Top-256 and preserves
   original vocabulary IDs through the proposal sampler. This makes the proposal
   q more faithful when repeat penalties demote tokens near top-k. Target p/q
   rejection verification remains exact, so this cannot approximate target output.
3. Selects raw Top-256 for the final target verification frontier as well as
   intermediate rows. Full frontier logits are STILL downloaded/retained exactly.
4. Caches that raw target Top-256 per session. On the next cycle, if the same
   strict certificate proves the request's post-penalty top-k support is wholly
   inside those candidates, the target anchor is sampled from the compact exact
   distribution instead of rescanning all 248,320 logits on the CPU. Certificate
   failure consumes no RNG and falls back to the unchanged full-vocabulary sampler.
5. Deferred residual samples always take precedence, preserving rejection/RNG
   behavior. Snapshot/restore and any new feed invalidate the performance cache.
6. Keeps normal HIP graph replay and the LLVM 23 gfx1151 W8A8 scheduler barrier.

Expected signal
---------------
Do not expect a guaranteed number before hardware measurement. The intended
benefits are (a) recover the pre-v6 controller baseline, (b) improve p/q overlap
and MTP acceptance where Top-64 was too narrow after penalties, and (c) cut most
of the measured CPU sampling scan cost when the strict Top-256 certificate passes.

After BUILD.bat, run the same benchmark at MTP max 6 / confidence 0.70. Look for:
- MTP cost profile: Gufo calibrated
- MTP draft candidate support: 256
- Compact frontier samples > 0
- Frontier certificate fallbacks
Then compare decode tok/s, acceptance, accepted/output, avg chosen depth, and
sampling ms against the valid 32.70 tok/s baseline.
