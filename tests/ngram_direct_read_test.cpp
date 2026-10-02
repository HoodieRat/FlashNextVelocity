#include "src/models/qwen38_flash_next/ngram.hpp"
#include "src/models/qwen38_flash_next/weights.hpp"
#include "src/core/quant/ggml_dequant.hpp"
#include "win_file.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  using gufo::models::qwen38_flash_next::NgramTable;
  constexpr std::uint32_t kRows = 18;
  constexpr std::uint32_t kDim = 160;
  const fs::path path = fs::temp_directory_path() / "flashnext_ngram_direct_read.bin";
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    for (std::uint32_t row = 0; row < kRows; ++row) {
      const std::uint16_t bits = static_cast<std::uint16_t>((127 + row) << 7);
      for (std::uint32_t col = 0; col < kDim; ++col) {
        file.write(reinterpret_cast<const char*>(&bits), sizeof(bits));
      }
    }
  }
  const int fd = gufo::win::OpenRead(path);
  if (fd < 0) {
    std::cerr << "open failed\n";
    return 1;
  }
  std::string error;
  auto table = NgramTable::Open(fd, 0, kRows, kDim,
                                gufo::core::GgmlType::kBF16, &error);
  gufo::win::Close(fd);
  if (!table) {
    std::cerr << "table open failed: " << error << '\n';
    return 1;
  }
  for (std::uint32_t width : {6U, 16U}) {
    for (int repeat = 0; repeat < 100; ++repeat) {
      std::vector<std::uint32_t> rows(width);
      std::vector<float> out(width * kDim);
      for (std::uint32_t i = 0; i < width; ++i) rows[i] = kRows - width + i;
      if (!table->Read(rows, out)) {
        std::cerr << "gather failed width=" << width << " repeat=" << repeat << '\n';
        return 1;
      }
      for (std::uint32_t i = 0; i < width; ++i) {
        const float expected = static_cast<float>(1U << rows[i]);
        for (std::uint32_t col = 0; col < kDim; ++col) {
          if (out[i * kDim + col] != expected) {
            std::cerr << "mismatch width=" << width << " row=" << rows[i] << '\n';
            return 1;
          }
        }
      }
    }
  }
  table.reset();
  fs::remove(path);
  if (argc > 1) {
    auto reader = gufo::core::GgufReader::OpenFile(argv[1], &error);
    if (!reader) { std::cerr << error << '\n'; return 1; }
    auto weights = gufo::models::qwen38_flash_next::ModelWeights::Bind(*reader, &error);
    if (!weights) { std::cerr << error << '\n'; return 1; }
    const auto& tensor = weights->ple_table;
    const int shard_fd = reader->GetMappedRegions()[tensor.shard].file_descriptor;
    auto production = NgramTable::Open(shard_fd, tensor.file_offset, tensor.rows,
                                       weights->config.ple_head_dim, tensor.type, &error);
    if (!production) { std::cerr << error << '\n'; return 1; }
    std::cout << "production rows=" << tensor.rows
              << " offset=" << tensor.file_offset
              << " row_bytes=" << production->RowBytes()
              << " shard=" << tensor.shard << '\n';
    std::uint64_t state = 0xA1B2C3D4E5F60789ULL;
    const int repeats = argc > 2 ? std::atoi(argv[2]) : 200;
    for (std::uint32_t width : {6U, 16U}) {
      for (int repeat = 0; repeat < repeats; ++repeat) {
        const std::uint32_t count = (width + 1) * weights->config.ple_heads;
        std::vector<std::uint32_t> rows(count);
        std::vector<float> out(count * production->RowDim());
        for (std::uint32_t i = 0; i < count; ++i) {
          state ^= state << 13; state ^= state >> 7; state ^= state << 17;
          rows[i] = static_cast<std::uint32_t>(state % tensor.rows);
        }
        rows.back() = static_cast<std::uint32_t>(tensor.rows - 1 - repeat % 8);
        if (!production->Read(rows, out)) {
          std::cerr << "production gather failed width=" << width
                    << " repeat=" << repeat << '\n';
          return 1;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
          std::vector<std::uint8_t> raw(production->RowBytes());
          const auto file_at = tensor.file_offset +
              std::uint64_t{rows[i]} * production->RowBytes();
          if (gufo::win::PRead(shard_fd, raw.data(), raw.size(), file_at) !=
              static_cast<std::int64_t>(raw.size())) {
            std::cerr << "buffered reference read failed\n";
            return 1;
          }
          std::vector<float> expected(production->RowDim());
          if (tensor.type == gufo::core::GgmlType::kIQ4_NL) {
            gufo::quant::DequantizeIQ4_NL(raw.data(), expected.data(), expected.size());
          } else {
            for (std::size_t col = 0; col < expected.size(); ++col) {
              std::uint16_t bits;
              std::memcpy(&bits, raw.data() + 2 * col, 2);
              const std::uint32_t f = static_cast<std::uint32_t>(bits) << 16;
              std::memcpy(&expected[col], &f, 4);
            }
          }
          for (std::size_t col = 0; col < expected.size(); ++col) {
            if (!std::isfinite(out[i * expected.size() + col]) ||
                out[i * expected.size() + col] != expected[col]) {
              std::cerr << "production mismatch width=" << width
                        << " repeat=" << repeat << " row=" << rows[i] << '\n';
              return 1;
            }
          }
        }
      }
      std::cout << "production width=" << width << " PASS\n";
    }
  }
  std::cout << "PASS\n";
}
