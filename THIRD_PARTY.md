# Third-party software

## Gufo

Source: https://github.com/pixmaate/gufo (`windows-port`)

Pinned revision: `cff564964e8506c0abb3e530cecb55332187ac6c` (integrated 2026-09-26)

License: MIT. The production build keeps the pinned Gufo checkout, including its license files, in the external `%LOCALAPPDATA%\FlashNextVelocity` source cache. The prior `.deps/gufo` checkout is preserved separately.

FlashNextVelocity uses Gufo's Qwen3.8 Flash-Next engine, MTP policy, Qwen tokenizer/template/vision path, HIP kernels, and MMQ-derived kernel package. Project-owned I/O, sampled-MTP verification, profiling, and native context-lookup proposal logic remain layered above this pinned Windows base.

## AMD ROCm / TheRock

The bootstrap downloads AMD's official Windows `gfx1151` ROCm 10.0.0 tarball. ROCm components retain AMD's licenses in the extracted package.

## vcpkg dependencies

ICU, OpenSSL, libpng, libjpeg-turbo, cURL, and their transitive dependencies are installed by vcpkg and retain their own licenses.


## Research inspiration

The native context-lookup and speculative-economics work was informed by published benchmark methodology and experiments in MKM-030/strix-alloy and MKM-030/strix-alloy-wsl2. FlashNextVelocity does not import their WSL/DXG bridge or Linux memory hooks; the implementation here is native Windows and independently integrated into the Gufo-based runtime.
