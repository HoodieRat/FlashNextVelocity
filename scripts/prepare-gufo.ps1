param(
  [Parameter(Mandatory=$true)][string]$GufoRoot
)
$ErrorActionPreference='Stop'
$Utf8NoBom=New-Object System.Text.UTF8Encoding($false)

function N([AllowEmptyString()][string]$s){ if($null -eq $s){return ''}; return $s.Replace("`r`n","`n").Replace("`r","`n") }
function Put([string]$p,[string]$s){ [IO.File]::WriteAllText($p,$s,$script:Utf8NoBom) }
function Count([string]$s,[string]$n){$c=0;$o=0;while($true){$i=$s.IndexOf($n,$o,[StringComparison]::Ordinal);if($i -lt 0){break};$c++;$o=$i+$n.Length};return $c}
function Replace-One([string]$Path,[AllowEmptyString()][string]$Old,[AllowEmptyString()][string]$New){
  $s=N([IO.File]::ReadAllText($Path));$o=N($Old);$n=N($New);$c=Count $s $o
  if($c -ne 1){throw "Source integration mismatch in ${Path}. Expected 1 occurrence, found $c.`nNeedle:`n$o"}
  Put $Path ($s.Replace($o,$n))
}
# Retain the GGUF reader's overlapped handle for FlashNextVelocity's PLE reads.
# Gufo's Win32 POSIX layer owns mapping, stat, close, and all other file I/O.
$p=Join-Path $GufoRoot 'src\core\gguf_reader.cpp'
Replace-One $p '#include "src/core/gguf_reader.hpp"' @'
#include "src/core/gguf_reader.hpp"
#include "win_file.hpp"
'@
Replace-One $p '  const int fd = open(path.c_str(), O_RDONLY);' '  const int fd = gufo::win::OpenRead(path);'

# Standard F16 Qwen mmproj support. Gufo's working kernels stay BF16/F32; the
# sidecar is converted once during lazy vision upload.
$p=Join-Path $GufoRoot 'src\models\qwen\vision\encoder.hip'
Replace-One $p '#include "src/core/gguf_reader.hpp"' @'
#include "src/core/gguf_reader.hpp"
#include "src/core/quant/ggml_dequant.hpp"
'@

Replace-One $p @'
std::uint16_t ToBf16(float value) {
  const auto bits = std::bit_cast<std::uint32_t>(value);
  return static_cast<std::uint16_t>((bits + 0x7fff + ((bits >> 16) & 1)) >> 16);
}
'@ @'
std::uint16_t ToBf16(float value) {
  const auto bits = std::bit_cast<std::uint32_t>(value);
  return static_cast<std::uint16_t>((bits + 0x7fff + ((bits >> 16) & 1)) >> 16);
}

float VisionScalar(const core::GgufTensorInfo* tensor, std::size_t index) {
  if (tensor->type == core::GgmlType::kF32)
    return static_cast<const float*>(tensor->data)[index];
  if (tensor->type == core::GgmlType::kF16)
    return gufo::quant::Fp16ToFloat(
        static_cast<const std::uint16_t*>(tensor->data)[index]);
  throw std::invalid_argument("unsupported scalar vision tensor type");
}

bool VisionMatrixBf16(std::string_view name) {
  if (name == "mm.0.weight" || name == "mm.2.weight") return true;
  return name.find("attn_qkv.weight") != std::string_view::npos ||
         name.find("attn_out.weight") != std::string_view::npos ||
         name.find("ffn_up.weight") != std::string_view::npos ||
         name.find("ffn_down.weight") != std::string_view::npos;
}
'@

Replace-One $p @'
      if (tensor == nullptr ||
          tensor->dimensions != std::vector<std::uint64_t>(shape) ||
          tensor->type != type) {
        throw std::invalid_argument("invalid BF16 Qwen vision tensor: " + name);
      }
      const auto bytes =
          tensor->ElementCount() * (type == core::GgmlType::kF32 ? 4 : 2);
'@ @'
      const bool compatible_type =
          tensor != nullptr &&
          (tensor->type == type || tensor->type == core::GgmlType::kF16);
      if (!compatible_type ||
          tensor->dimensions != std::vector<std::uint64_t>(shape)) {
        throw std::invalid_argument("invalid F16/BF16 Qwen vision tensor: " + name);
      }
      const auto bytes = tensor->ElementCount() *
                         (tensor->type == core::GgmlType::kF32 ? 4 : 2);
'@

Replace-One $p @'
      const std::size_t bytes =
          tensor.ElementCount() * (tensor.type == core::GgmlType::kF32 ? 4 : 2);
      Allocation buffer(bytes);
      bytes_uploaded += bytes;
      Check(hipMemcpyAsync(buffer.data, tensor.data, bytes,
                           hipMemcpyHostToDevice, stream));
      weights.emplace(tensor.name, std::move(buffer));
'@ @'
      const auto count = static_cast<std::size_t>(tensor.ElementCount());
      if (VisionMatrixBf16(tensor.name)) {
        const std::size_t bytes = count * sizeof(std::uint16_t);
        Allocation buffer(bytes);
        bytes_uploaded += bytes;
        if (tensor.type == core::GgmlType::kF16) {
          const auto* source = static_cast<const std::uint16_t*>(tensor.data);
          std::vector<std::uint16_t> converted(count);
          for (std::size_t i = 0; i < count; ++i)
            converted[i] = ToBf16(gufo::quant::Fp16ToFloat(source[i]));
          Check(hipMemcpyAsync(buffer.data, converted.data(), bytes,
                               hipMemcpyHostToDevice, stream));
          Check(hipStreamSynchronize(stream));
        } else if (tensor.type == core::GgmlType::kBF16) {
          Check(hipMemcpyAsync(buffer.data, tensor.data, bytes,
                               hipMemcpyHostToDevice, stream));
        } else {
          throw std::invalid_argument("vision matrix must be F16 or BF16: " +
                                      std::string(tensor.name));
        }
        weights.emplace(tensor.name, std::move(buffer));
      } else {
        const std::size_t bytes = count * sizeof(float);
        Allocation buffer(bytes);
        bytes_uploaded += bytes;
        if (tensor.type == core::GgmlType::kF16) {
          const auto* source = static_cast<const std::uint16_t*>(tensor.data);
          std::vector<float> converted(count);
          for (std::size_t i = 0; i < count; ++i)
            converted[i] = gufo::quant::Fp16ToFloat(source[i]);
          Check(hipMemcpyAsync(buffer.data, converted.data(), bytes,
                               hipMemcpyHostToDevice, stream));
          Check(hipStreamSynchronize(stream));
        } else if (tensor.type == core::GgmlType::kF32) {
          Check(hipMemcpyAsync(buffer.data, tensor.data, bytes,
                               hipMemcpyHostToDevice, stream));
        } else {
          throw std::invalid_argument("vision scalar tensor must be F16 or F32: " +
                                      std::string(tensor.name));
        }
        weights.emplace(tensor.name, std::move(buffer));
      }
'@

Replace-One $p @'
    const auto* first = static_cast<const float*>(
        reader->FindTensor("v.patch_embd.weight")->data);
    const auto* second = static_cast<const float*>(
        reader->FindTensor("v.patch_embd.weight.1")->data);
    std::vector<std::uint16_t> patch(kHidden * kPatchDim);
    for (unsigned h = 0; h < kHidden; ++h) {
      for (unsigned c = 0; c < 3; ++c) {
        for (unsigned t = 0; t < 2; ++t) {
          for (unsigned p = 0; p < 256; ++p) {
            patch[h * kPatchDim + (c * 2 + t) * 256 + p] =
                ToBf16((t == 0 ? first : second)[(h * 3 + c) * 256 + p]);
          }
        }
      }
    }
'@ @'
    const auto* first = reader->FindTensor("v.patch_embd.weight");
    const auto* second = reader->FindTensor("v.patch_embd.weight.1");
    std::vector<std::uint16_t> patch(kHidden * kPatchDim);
    for (unsigned h = 0; h < kHidden; ++h) {
      for (unsigned c = 0; c < 3; ++c) {
        for (unsigned t = 0; t < 2; ++t) {
          for (unsigned p = 0; p < 256; ++p) {
            const auto index = (h * 3 + c) * 256 + p;
            patch[h * kPatchDim + (c * 2 + t) * 256 + p] =
                ToBf16(VisionScalar(t == 0 ? first : second, index));
          }
        }
      }
    }
'@

# Keep upstream BF16 auto-discovery only. F16 is explicit through config, so a
# blank mmproj genuinely disables vision when troubleshooting.

Write-Host 'Integrated Windows/Strix-Halo source preparation complete.' -ForegroundColor Green
