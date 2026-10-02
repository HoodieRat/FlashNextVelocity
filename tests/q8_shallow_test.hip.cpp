// Compares the shallow Q8_0 verification kernel with the existing MMVQ
// path. Outputs must be bit-identical. Also times N = 2/3/4 projections.
#include "qfn_mmq.h"

#include <hip/hip_runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

#define QK8_0 32
#define QK8_1 32

struct BlockQ8_0 {
  uint16_t d;
  int8_t qs[QK8_0];
};
struct BlockQ8_1 {
  uint16_t d;
  uint16_t s;
  int8_t qs[QK8_1];
};
static_assert(sizeof(BlockQ8_0) == 34, "q8_0 layout");
static_assert(sizeof(BlockQ8_1) == 36, "q8_1 layout");

int Pad(int k) { return (k + 512 - 1) / 512 * 512; }

uint16_t HalfBits(int nibble) {
  // Small finite fp16 values, including a negative scale.
  static const uint16_t k[] = {0x3C00, 0x3800, 0x3400, 0xBC00,
                                0x4000, 0x4200, 0x2E00, 0x0000};
  return k[nibble & 7];
}

bool Check(hipError_t err, const char* what) {
  if (err == hipSuccess) return true;
  std::fprintf(stderr, "%s: %s\n", what, hipGetErrorString(err));
  return false;
}

struct Case {
  const char* name;
  int m;
  int k;
  bool gate;
  bool time_it;
};

int Compare(const Case& c, int n, bool quiet) {
  const int blocks = c.k / QK8_0;
  const int stride = Pad(c.k) / QK8_1;
  const size_t w_blocks = static_cast<size_t>(c.m) * blocks;
  const size_t x_blocks = static_cast<size_t>(n) * stride;
  std::vector<BlockQ8_0> w(w_blocks), g(c.gate ? w_blocks : 0);
  std::vector<BlockQ8_1> x(x_blocks);
  uint32_t seed = 0x9E3779B9u ^ (static_cast<uint32_t>(c.m) * 131u) ^
                  (static_cast<uint32_t>(c.k) << 8) ^ static_cast<uint32_t>(n);
  auto next = [&]() {
    seed = seed * 1664525u + 1013904223u;
    return seed;
  };
  for (size_t i = 0; i < w_blocks; ++i) {
    w[i].d = HalfBits(static_cast<int>(next()));
    for (int q = 0; q < QK8_0; ++q)
      w[i].qs[q] = static_cast<int8_t>(static_cast<int>(next() % 255) - 127);
    if (c.gate) {
      g[i].d = HalfBits(static_cast<int>(next() >> 3));
      for (int q = 0; q < QK8_0; ++q)
        g[i].qs[q] = static_cast<int8_t>(static_cast<int>(next() % 201) - 100);
    }
  }
  for (int t = 0; t < n; ++t) {
    for (int b = 0; b < blocks; ++b) {
      auto& row = x[static_cast<size_t>(t) * stride + b];
      row.d = HalfBits(static_cast<int>(next() >> 1));
      row.s = HalfBits(static_cast<int>(next() >> 2));
      for (int q = 0; q < QK8_1; ++q)
        row.qs[q] = static_cast<int8_t>(static_cast<int>(next() % 181) - 90);
    }
  }
  BlockQ8_0 *dw = nullptr, *dg = nullptr;
  BlockQ8_1* dx = nullptr;
  float *got = nullptr, *ref = nullptr;
  const size_t out_n = static_cast<size_t>(n) * c.m;
  if (!Check(hipMalloc(&dw, w.size() * sizeof(BlockQ8_0)), "w") ||
      !Check(hipMalloc(&dx, x.size() * sizeof(BlockQ8_1)), "x") ||
      !Check(hipMalloc(&got, out_n * sizeof(float)), "got") ||
      !Check(hipMalloc(&ref, out_n * sizeof(float)), "ref") ||
      !Check(hipMemcpy(dw, w.data(), w.size() * sizeof(BlockQ8_0),
                       hipMemcpyHostToDevice),
             "w copy") ||
      !Check(hipMemcpy(dx, x.data(), x.size() * sizeof(BlockQ8_1),
                       hipMemcpyHostToDevice),
             "x copy")) {
    return 1;
  }
  if (c.gate) {
    if (!Check(hipMalloc(&dg, g.size() * sizeof(BlockQ8_0)), "g") ||
        !Check(hipMemcpy(dg, g.data(), g.size() * sizeof(BlockQ8_0),
                         hipMemcpyHostToDevice),
               "g copy"))
      return 1;
  }
  hipMemset(got, 0x7F, out_n * sizeof(float));
  hipMemset(ref, 0x5A, out_n * sizeof(float));
  const void* gate = c.gate ? dg : nullptr;
  if (qfn_q8_mmvq_reference(dw, gate, dx, ref, c.m, n, c.k, nullptr) != 0 ||
      qfn_q8_shallow_vec(dw, gate, dx, got, c.m, n, c.k, nullptr) != 0 ||
      !Check(hipDeviceSynchronize(), "sync")) {
    std::fprintf(stderr, "launch failed %s N=%d\n", c.name, n);
    return 1;
  }
  std::vector<float> hg(out_n), hr(out_n);
  hipMemcpy(hg.data(), got, out_n * sizeof(float), hipMemcpyDeviceToHost);
  hipMemcpy(hr.data(), ref, out_n * sizeof(float), hipMemcpyDeviceToHost);
  int mismatches = 0;
  for (size_t i = 0; i < out_n; ++i) {
    if (std::memcmp(&hg[i], &hr[i], sizeof(float)) != 0) {
      if (mismatches < 4) {
        std::fprintf(stderr, "%s N=%d [%zu] got %.9g ref %.9g\n", c.name, n, i,
                     hg[i], hr[i]);
      }
      ++mismatches;
    }
  }
  double us_new = 0, us_old = 0;
  if (c.time_it && mismatches == 0) {
    hipEvent_t a, b;
    hipEventCreate(&a);
    hipEventCreate(&b);
    const int warm = 5, reps = 20;
    for (int i = 0; i < warm; ++i)
      qfn_q8_shallow_vec(dw, gate, dx, got, c.m, n, c.k, nullptr);
    hipEventRecord(a, nullptr);
    for (int i = 0; i < reps; ++i)
      qfn_q8_shallow_vec(dw, gate, dx, got, c.m, n, c.k, nullptr);
    hipEventRecord(b, nullptr);
    hipEventSynchronize(b);
    float ms = 0;
    hipEventElapsedTime(&ms, a, b);
    us_new = ms * 1000.0 / reps;
    for (int i = 0; i < warm; ++i)
      qfn_q8_mmvq_reference(dw, gate, dx, ref, c.m, n, c.k, nullptr);
    hipEventRecord(a, nullptr);
    for (int i = 0; i < reps; ++i)
      qfn_q8_mmvq_reference(dw, gate, dx, ref, c.m, n, c.k, nullptr);
    hipEventRecord(b, nullptr);
    hipEventSynchronize(b);
    hipEventElapsedTime(&ms, a, b);
    us_old = ms * 1000.0 / reps;
    hipEventDestroy(a);
    hipEventDestroy(b);
    const double bytes = static_cast<double>(w_blocks) * sizeof(BlockQ8_0) *
                         (c.gate ? 2 : 1);
    std::printf("%-18s N=%d  old %8.1f us  new %8.1f us  %+6.1f%%  (%.0f MB weights)\n",
                c.name, n, us_old, us_new, (us_old - us_new) / us_old * 100.0,
                bytes / 1e6);
  } else if (!quiet) {
    std::printf("%-18s N=%d  %s\n", c.name, n,
                mismatches ? "MISMATCH" : "bit-identical");
  }
  hipFree(dw);
  hipFree(dx);
  hipFree(got);
  hipFree(ref);
  if (dg) hipFree(dg);
  return mismatches == 0 ? 0 : 1;
}

}  // namespace

int main() {
  if (hipSetDevice(0) != hipSuccess) {
    std::fprintf(stderr, "no HIP device\n");
    return 1;
  }
  const Case correctness[] = {
      {"tail-k32", 1, 32, false, false},
      {"tail-k96-m7", 7, 96, false, false},
      {"tail-k160-m13", 13, 160, false, false},
      {"tail-k320-m3", 3, 320, false, false},
      {"tail-k64-m8", 8, 64, true, false},
      {"hc-up", 128, 320, false, false},
      {"hc-down", 64, 10240, false, false},
      {"attn-kv", 64, 2560, false, false},
      {"attn-out", 48, 6144, false, false},
      {"shexp-gate", 40, 640, true, false},
      {"shexp-down", 40, 2560, false, false},
  };
  const Case timed[] = {
      {"attn-q", 12288, 2560, false, true},
      {"attn-kv", 512, 2560, false, true},
      {"attn-out", 2560, 6144, false, true},
      {"hc-up", 10240, 320, false, true},
      {"hc-down", 320, 10240, false, true},
      {"shexp-gate", 640, 2560, true, true},
      {"shexp-up", 640, 2560, true, true},
      {"vocab", 8192, 2560, false, true},
  };
  int failed = 0;
  for (const auto& c : correctness) {
    for (int n = 1; n <= 4; ++n) failed += Compare(c, n, false);
  }
  for (const auto& c : timed) {
    for (int n = 2; n <= 4; ++n) failed += Compare(c, n, false);
  }
  // Production HC down path versus the previous MMVQ, N = 2/3/4.
  for (int n = 2; n <= 4; ++n) {
    const int m = 320, k = 10240;
    const int blocks = k / 32;
    const int stride = Pad(k) / 32;
    std::vector<BlockQ8_0> w(static_cast<size_t>(m) * blocks);
    std::vector<BlockQ8_1> x(static_cast<size_t>(n) * stride);
    for (size_t i = 0; i < w.size(); ++i) {
      w[i].d = 0x3C00;
      for (int q = 0; q < 32; ++q) w[i].qs[q] = static_cast<int8_t>((i + q) % 17 - 8);
    }
    for (size_t i = 0; i < x.size(); ++i) {
      x[i].d = 0x3800;
      x[i].s = 0;
      for (int q = 0; q < 32; ++q) x[i].qs[q] = static_cast<int8_t>(q - 16);
    }
    BlockQ8_0* dw = nullptr;
    BlockQ8_1* dx = nullptr;
    float *hc = nullptr, *ref = nullptr;
    hipMalloc(&dw, w.size() * sizeof(BlockQ8_0));
    hipMalloc(&dx, x.size() * sizeof(BlockQ8_1));
    hipMalloc(&hc, static_cast<size_t>(n) * m * sizeof(float));
    hipMalloc(&ref, static_cast<size_t>(n) * m * sizeof(float));
    hipMemcpy(dw, w.data(), w.size() * sizeof(BlockQ8_0), hipMemcpyHostToDevice);
    hipMemcpy(dx, x.data(), x.size() * sizeof(BlockQ8_1), hipMemcpyHostToDevice);
    qfn_mmq_q8_0_dense_vec_preq(dw, nullptr, dx, hc, m, n, k, nullptr);
    qfn_q8_mmvq_reference(dw, nullptr, dx, ref, m, n, k, nullptr);
    hipDeviceSynchronize();
    std::vector<float> a(static_cast<size_t>(n) * m), b = a;
    hipMemcpy(a.data(), hc, a.size() * sizeof(float), hipMemcpyDeviceToHost);
    hipMemcpy(b.data(), ref, b.size() * sizeof(float), hipMemcpyDeviceToHost);
    int bad = 0;
    for (size_t i = 0; i < a.size(); ++i)
      bad += std::memcmp(&a[i], &b[i], sizeof(float)) != 0;
    std::printf("hc-down production N=%d vs MMVQ  %s\n", n,
                bad ? "MISMATCH" : "bit-identical");
    failed += bad ? 1 : 0;
    hipFree(dw);
    hipFree(dx);
    hipFree(hc);
    hipFree(ref);
  }
  if (failed) {
    std::fprintf(stderr, "Q8 shallow operator tests failed: %d\n", failed);
    return 1;
  }
  std::printf("Q8 shallow operator tests passed\n");
  return 0;
}
