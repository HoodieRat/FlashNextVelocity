# Third-party software and source credits

FlashNextVelocity is maintained as a separate Windows application and inference project. Its original contributions are covered by the root [MIT license](LICENSE). Incorporated code retains the original authors' copyright and license terms. Attribution is not a claim of affiliation or endorsement.

## Incorporated source

### Gufo and its Windows port

- Original project: [gufo-org/gufo](https://github.com/gufo-org/gufo).
- Production source: [pixmaate/gufo](https://github.com/pixmaate/gufo), `windows-port` lineage, pinned to **`cff564964e8506c0abb3e530cecb55332187ac6c`** by `scripts/build.ps1`.
- Relationship: the Qwen3.8 Flash-Next inference base, MTP/model support, tokenizer/template/vision paths and HIP execution. Windows preparation, `gufo-overlay/` and project serving code integrate FlashNextVelocity changes into that base.
- License: **MIT**, Copyright (c) 2026 Gufo contributors. The project license does not replace this notice.
- Retained verbatim: [LICENSE](licenses/gufo/LICENSE), [NOTICE](licenses/gufo/NOTICE), [upstream third-party inventory](licenses/gufo/THIRD_PARTY_NOTICES.md) and [imported license texts](licenses/gufo/imported).

The build caches the prepared pinned source under `%LOCALAPPDATA%\FlashNextVelocity`. Notices are also retained in this repository and build packages, so attribution does not depend on that local cache.

**Scope of the retained upstream inventory:** it describes Gufo's wider project, including other model families, Linux/Nix dependencies and optional FFmpeg tools. It is preserved as an upstream record; its Linux package versions are not the versions shipped by this Windows build. This repository does not bundle model weights or FFmpeg executables. The Windows components are listed below.

### llama.cpp / ggml

- Source: [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp).
- Relationship: quantization/dequantization and model-private HIP/MMQ kernels adapted through the incorporated source base.
- MMQ import pin: **`5c0e9468378eba6bf3cc1989ff5d62fbbe4d9e3a`**; the upstream inventory separately records the attention import at `e9fa0781f1c25fc4fe8c86be1edc6970661ad6f0`.
- License: **MIT**, Copyright (c) 2023–2026 The ggml authors. Retained [license text](licenses/gufo/imported/llama.cpp.txt), with [VENDOR.md](gufo-overlay/src/models/qwen38_flash_next/kernels/rocm/mmq/VENDOR.md) and [LICENSE](gufo-overlay/src/models/qwen38_flash_next/kernels/rocm/mmq/LICENSE) next to the adapted kernels.

`VENDOR.md` is retained verbatim from the pinned source; its upstream Gufo/Nix maintenance instructions describe that source's workflow.

### Qwen reference material

The incorporated source identifies Qwen reference chat templates and adapted renderer behavior under **Apache-2.0**. The [Qwen notice/license](licenses/gufo/imported/qwen.txt) and upstream inventory retain that attribution. FlashNextVelocity model and sidecar files are supplied by the user; their publishers' model licenses apply separately.

## Windows SDK and runtime dependencies

The bootstrap selects AMD's official [Windows gfx1151 ROCm 10.0.0 package](https://stable.repo.amd.com/rocm/core/tarball/therock-dist-windows-gfx1151-10.0.0.tar.gz). The benchmark's loaded HIP runtime reports file version `10.0.3581.0`. These are package/runtime identities, not a claim that every contained library has the same version or license.

| Component | Relationship / source | Retained terms |
| --- | --- | --- |
| AMD ROCm / HIP, hipBLAS, hipBLASLt, rocBLAS and supplied SDK components | Windows HIP build/runtime package; [ROCm/TheRock](https://github.com/ROCm/TheRock) | [Supplied SDK notices](licenses/runtime/rocm); upstream [HIP](licenses/gufo/imported/hip.txt), [hipBLAS](licenses/gufo/imported/hipblas.txt), [hipBLASLt](licenses/gufo/imported/hipblaslt.txt), [rocBLAS](licenses/gufo/imported/rocblas.txt) notices |
| hipCUB | Headers used in compiled kernels; [ROCm/hipCUB](https://github.com/ROCm/hipCUB) | [BSD-3-Clause](licenses/gufo/imported/hipcub.txt), retaining Duane Merrill, NVIDIA and AMD notices |
| rocPRIM, rocWMMA, Composable Kernel | Kernel/header work identified by the incorporated source | [rocPRIM](licenses/gufo/imported/rocprim.txt), [rocWMMA](licenses/gufo/imported/rocwmma.txt), [Composable Kernel](licenses/gufo/imported/composable_kernel.txt) |
| ICU | Unicode/tokenizer support; [unicode-org/icu](https://github.com/unicode-org/icu) | [Supplied copyright/license](licenses/runtime/vcpkg/icu/copyright) |
| OpenSSL | Hashing/TLS; [openssl/openssl](https://github.com/openssl/openssl) | [Supplied copyright/license](licenses/runtime/vcpkg/openssl/copyright) |
| cURL | HTTPS/image retrieval; [curl/curl](https://github.com/curl/curl) | [Supplied copyright/license](licenses/runtime/vcpkg/curl/copyright) |
| libpng | PNG decoding; [pnggroup/libpng](https://github.com/pnggroup/libpng) | [Supplied copyright/license](licenses/runtime/vcpkg/libpng/copyright) |
| libjpeg-turbo | JPEG decoding; [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) | [IJG, BSD and zlib notices](licenses/runtime/vcpkg/libjpeg-turbo/copyright) |
| libspng, zlib | Installed transitive runtime dependencies | [libspng](licenses/runtime/vcpkg/libspng/copyright), [zlib](licenses/runtime/vcpkg/zlib/copyright) |

**This software is based in part on the work of the Independent JPEG Group.**

The `licenses/runtime/` files are notices copied from the SDK and vcpkg installation used here, not substitute license texts written by this project. `scripts/copy-third-party-notices.ps1` includes the repository notices and copies supplied notices from the selected SDK/dependency installation into `dist/third-party/` and candidate packages. Changes to bundled components require checking and retaining the notices for those actual components; the project's MIT license does not relicense them.

## Implementation references and research credits

These credits identify algorithm or architecture references used in the project's recorded optimization work. They are distinct from the incorporated inference base and adapted kernel sources above.

| Project | Documented role |
| --- | --- |
| [halo-box/strix-llama.cpp](https://github.com/halo-box/strix-llama.cpp), preserved revision `7449a0fe…` | Deterministic draft top-token proposal comparison in [v1.0.6 notes](OPTIMIZATION-PASS-v1.0.6.md); inherited llama.cpp authorship is retained in the ggml MIT notice |
| Historical `halo-box/llama-halo-hybrid` and preserved Halo/Strix Qwen4Exp graph | MTP state/rollback architecture comparisons and per-HC-stream normalization reference; see [v1.0.8](OPTIMIZATION-PASS-v1.0.8.md) and [normalization notes](MTP-HC-GROUPNORM-v1.0.9.md). The historical repository is not currently publicly accessible at its recorded URL |
| [MKM-030/strix-alloy](https://github.com/MKM-030/strix-alloy), [strix-alloy-wsl2](https://github.com/MKM-030/strix-alloy-wsl2) | Benchmark methodology, lookup/speculation economics and memory-placement comparisons; this implementation uses native Windows HIP |
| [peonist-ai/halogen-flash-server](https://github.com/peonist-ai/halogen-flash-server) | Inference architecture/performance reference; no claim of a Halogen backend or wholesale code import |

Gufo's own optimization acknowledgments include [LaurentZuijdwijk/llama.cpp](https://github.com/LaurentZuijdwijk/llama.cpp), [Nathanw1014/strix-halo-llamacpp](https://github.com/Nathanw1014/strix-halo-llamacpp) and [gaetan-puleo/llama-cpp-strix-halo](https://github.com/gaetan-puleo/llama-cpp-strix-halo). Those original credits, pinned notice links and Nathan Wilson's copyright are preserved in the [upstream inventory](licenses/gufo/THIRD_PARTY_NOTICES.md) and [imported notices](licenses/gufo/imported).
