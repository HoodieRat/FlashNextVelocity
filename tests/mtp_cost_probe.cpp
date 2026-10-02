// Dumps trunk prefill logits and optionally compares them with the F32 oracle.
// Use the production CLI for generation and the session test for replay.
//
// gpu_probe --model FIRST_SHARD.gguf --prompt TEXT [--reference]
//           [--batch T] [--context N] [--dump logits.bin]
// Independent predictor oracle: --mtp-model MTP.gguf --mtp-audit
// MTP cost calibration: --mtp-model MTP.gguf --cost-audit C (0 = all)
//                      [--depth N] (default: 0, 4096, 32768)
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "src/core/gguf_reader.hpp"
#include "src/models/qwen/tokenizer.hpp"
#include "src/models/qwen38_flash_next/kernels/rocm/device_model.hpp"
#include "src/models/qwen38_flash_next/kernels/rocm/executor.hpp"
#include "src/models/qwen38_flash_next/ngram.hpp"
#include "src/models/qwen38_flash_next/reference.hpp"
#include "src/models/qwen38_flash_next/weights.hpp"

namespace q = gufo::models::qwen38_flash_next;
std::vector<std::int32_t> LatinTextVocabulary(
    const gufo::tokenization::QwenTokenizer& tokenizer) {
  constexpr char32_t kTypographic[] = {
      0x2014, 0x2013, 0x201C, 0x201D, 0x2018, 0x2019, 0x2026, 0x2022, 0x2192,
      0x2190, 0x2248, 0x00D7, 0x00F7, 0x00B1, 0x00B0, 0x00B7, 0x00E9};
  const auto ascii = [](unsigned char b) {
    return (b >= 0x20 && b < 0x7F) || b == '\t' || b == '\n' || b == '\r';
  };
  std::vector<std::int32_t> ids;
  for (std::size_t id = 0; id < tokenizer.GetVocabSize(); ++id) {
    const auto token = static_cast<gufo::tokenization::TokenId>(id);
    if (tokenizer.IsSpecialToken(token)) {
      ids.push_back(static_cast<std::int32_t>(id));
      continue;
    }
    const std::string text = tokenizer.DecodeTokenCopy(token);
    bool keep = !text.empty();
    for (std::size_t i = 0; keep && i < text.size();) {
      const auto b = static_cast<unsigned char>(text[i]);
      if (b < 0x80) {
        keep = ascii(b);
        ++i;
        continue;
      }
      // A complete 2- or 3-byte UTF-8 sequence for an allowed mark.
      const std::size_t length = (b & 0xE0) == 0xC0   ? 2
                                 : (b & 0xF0) == 0xE0 ? 3
                                                      : 0;
      if (length == 0 || i + length > text.size()) {
        keep = false;
        break;
      }
      char32_t cp = length == 2 ? (b & 0x1F) : (b & 0x0F);
      for (std::size_t j = 1; j < length; ++j) {
        const auto c = static_cast<unsigned char>(text[i + j]);
        if ((c & 0xC0) != 0x80) {
          keep = false;
          break;
        }
        cp = (cp << 6) | (c & 0x3F);
      }
      keep = keep && std::find(std::begin(kTypographic), std::end(kTypographic),
                               cp) != std::end(kTypographic);
      i += length;
    }
    if (keep) {
      ids.push_back(static_cast<std::int32_t>(id));
    }
  }
  return ids;
}

void AuditMtp(q::rocm::Executor& executor, const q::rocm::DeviceModel& device,
              const q::ModelWeights& weights, const q::MtpWeights& mtp,
              const gufo::tokenization::QwenTokenizer& tokenizer,
              const std::filesystem::path& path);
void AuditMtpCosts(q::rocm::Executor& executor,
                   const gufo::tokenization::QwenTokenizer& tokenizer,
                   std::span<const std::uint32_t> depths,
                   std::uint32_t concurrency);

namespace {

struct Stats {
  std::uint32_t argmax_a;
  std::uint32_t argmax_b;
  double kl;
  double max_abs;
};

Stats Compare(const float* a, const float* b, std::size_t n) {
  std::vector<double> pa(n);
  std::vector<double> pb(n);
  const float ma = *std::max_element(a, a + n);
  const float mb = *std::max_element(b, b + n);
  double sa = 0.0;
  double sb = 0.0;
  double max_abs = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    pa[i] = std::exp(static_cast<double>(a[i] - ma));
    pb[i] = std::exp(static_cast<double>(b[i] - mb));
    sa += pa[i];
    sb += pb[i];
    max_abs = std::max(max_abs, std::fabs(static_cast<double>(a[i] - b[i])));
  }
  double kl = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    pa[i] /= sa;
    pb[i] /= sb;
    kl += pa[i] * (std::log(pa[i] + 1e-30) - std::log(pb[i] + 1e-30));
  }
  return {static_cast<std::uint32_t>(std::max_element(a, a + n) - a),
          static_cast<std::uint32_t>(std::max_element(b, b + n) - b), kl,
          max_abs};
}

}  // namespace

int main(int argc, char** argv) {
  std::string model_path;
  std::string prompt = "The capital of France is";
  std::string dump_path;
  std::string mtp_path;
  bool mtp_audit = false;
  bool reference = false;
  bool cost_audit = false;
  std::uint32_t cost_concurrency = 0;
  std::optional<std::uint32_t> cost_depth;
  std::uint32_t batch = 512;
  std::uint32_t context = 4096;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> std::string {
      return i + 1 < argc ? argv[++i] : std::string();
    };
    if (arg == "--model") {
      model_path = next();
    } else if (arg == "--mtp-model") {
      mtp_path = next();
    } else if (arg == "--mtp-audit") {
      mtp_audit = true;
    } else if (arg == "--cost-audit") {
      cost_audit = true;
      const auto value = next();
      const auto [end, ec] = std::from_chars(
          value.data(), value.data() + value.size(), cost_concurrency);
      if (ec != std::errc{} || end != value.data() + value.size() ||
          (cost_concurrency != 0 && cost_concurrency != 1 &&
           cost_concurrency != 2 && cost_concurrency != 4 &&
           cost_concurrency != 6 && cost_concurrency != 8)) {
        std::fprintf(stderr, "--cost-audit requires C=0/1/2/4/6/8 (0 = all)\n");
        return 2;
      }
    } else if (arg == "--depth") {
      const auto value = next();
      std::uint32_t depth = 0;
      const auto [end, ec] =
          std::from_chars(value.data(), value.data() + value.size(), depth);
      if (ec != std::errc{} || end != value.data() + value.size()) {
        std::fprintf(stderr, "--depth requires a nonnegative integer\n");
        return 2;
      }
      cost_depth = depth;
    } else if (arg == "--prompt") {
      prompt = next();
    } else if (arg == "--reference") {
      reference = true;
    } else if (arg == "--batch" || arg == "--context") {
      const auto value = next();
      auto& count = arg == "--batch" ? batch : context;
      const auto [end, ec] =
          std::from_chars(value.data(), value.data() + value.size(), count);
      if (ec != std::errc{} || end != value.data() + value.size() ||
          count == 0) {
        std::fprintf(stderr, "%s requires a positive integer\n", arg.c_str());
        return 2;
      }
    } else if (arg == "--dump") {
      dump_path = next();
    } else {
      std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
      return 2;
    }
  }
  if (model_path.empty()) {
    std::fprintf(stderr, "--model is required\n");
    return 2;
  }
  if ((mtp_audit || cost_audit) && mtp_path.empty()) {
    std::fprintf(stderr, "MTP audits require --mtp-model\n");
    return 2;
  }
  if (cost_audit && (mtp_audit || reference || !dump_path.empty())) {
    std::fprintf(stderr, "--cost-audit cannot be combined with other probes\n");
    return 2;
  }
  if (cost_depth && !cost_audit) {
    std::fprintf(stderr, "--depth requires --cost-audit\n");
    return 2;
  }
  std::string error;
  auto reader = gufo::core::GgufReader::OpenFile(model_path, &error);
  if (!reader) {
    std::fprintf(stderr, "open failed: %s\n", error.c_str());
    return 1;
  }
  auto weights = q::ModelWeights::Bind(*reader, &error);
  if (!weights) {
    std::fprintf(stderr, "bind failed: %s\n", error.c_str());
    return 1;
  }
  auto tokenizer =
      gufo::tokenization::QwenTokenizer::CreateFromGguf(*reader, &error);
  if (!tokenizer) {
    std::fprintf(stderr, "tokenizer failed: %s\n", error.c_str());
    return 1;
  }
  const auto& c = weights->config;
  if (cost_depth &&
      (c.context_length < 96 || *cost_depth > c.context_length - 96)) {
    std::fprintf(stderr, "--depth leaves no room for the cost-audit suffix\n");
    return 2;
  }
  std::unique_ptr<q::NgramTable> ngram;
  if (c.ple_layer >= 0) {
    const auto& t = weights->ple_table;
    ngram = q::NgramTable::Open(
        reader->GetMappedRegions()[t.shard].file_descriptor, t.file_offset,
        t.rows, c.ple_head_dim, t.type, &error);
    if (!ngram) {
      std::fprintf(stderr, "n-gram table failed: %s\n", error.c_str());
      return 1;
    }
  }

  std::shared_ptr<gufo::core::GgufReader> mtp_reader;
  std::optional<q::MtpWeights> mtp_weights;
  if (!mtp_path.empty()) {
    mtp_reader = gufo::core::GgufReader::OpenFile(mtp_path, &error);
    if (mtp_reader)
      mtp_weights = q::MtpWeights::Bind(*mtp_reader, c, &error);
    if (!mtp_weights) {
      std::fprintf(stderr, "MTP bind failed: %s\n", error.c_str());
      return 1;
    }
  }
  auto device = q::rocm::DeviceModel::Upload(
      *weights, *reader, mtp_weights ? &*mtp_weights : nullptr,
      mtp_reader.get(), &error);
  if (!device) {
    std::fprintf(stderr, "upload failed: %s\n", error.c_str());
    return 1;
  }
  q::rocm::Executor::Options options;
  options.max_batch = batch;
  options.max_logit_rows = std::min<std::uint32_t>(batch, 64);
  if (mtp_audit || cost_audit)
    options.max_speculative = 17;

  if (cost_audit) {
    options.max_batch = batch;
    options.draft_vocab = LatinTextVocabulary(*tokenizer);
    options.max_logit_rows = 17;
  }
  auto executor =
      q::rocm::Executor::Create(*device, ngram.get(), options, &error);
  if (!executor) {
    std::fprintf(stderr, "executor failed: %s\n", error.c_str());
    return 1;
  }
  if (cost_audit) {
    try {
      const std::array<std::uint32_t, 3> depths{0, 4096, 32672};
      std::span<const std::uint32_t> selected_depths = depths;
      if (cost_depth)
        selected_depths = std::span(&*cost_depth, 1);
      AuditMtpCosts(*executor, *tokenizer, selected_depths, cost_concurrency);
      return 0;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "cost audit failed: %s\n", e.what());
      return 1;
    }
  }
  return 2;
}
