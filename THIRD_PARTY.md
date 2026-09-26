# Third-party software

## Gufo

Source: https://github.com/gufo-org/gufo

Pinned revision: `9cad13974cf6da0cd3674b4e0a88b14b7e4a2908`

License: MIT. The bootstrap retains Gufo's `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md`, and `licenses/` files in `.deps/gufo`.

FlashNextVelocity uses Gufo's Qwen3.8 Flash-Next engine, MTP policy, Qwen tokenizer/template/vision path, HIP kernels, and MMQ-derived kernel package. Project-owned integration adds Windows platform I/O/mapping/network support plus sampled-MTP verification, profiling, and native context-lookup proposal logic on top of the pinned Gufo base.

## AMD ROCm / TheRock

The bootstrap downloads AMD's official Windows `gfx1151` ROCm 10.0.0 tarball. ROCm components retain AMD's licenses in the extracted package.

## vcpkg dependencies

ICU, OpenSSL, libpng, libjpeg-turbo, cURL, and their transitive dependencies are installed by vcpkg and retain their own licenses.


## Research inspiration

The native context-lookup and speculative-economics work was informed by published benchmark methodology and experiments in MKM-030/strix-alloy and MKM-030/strix-alloy-wsl2. FlashNextVelocity does not import their WSL/DXG bridge or Linux memory hooks; the implementation here is native Windows and independently integrated into the Gufo-based runtime.
