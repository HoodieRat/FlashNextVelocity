#ifndef GUFO_MODELS_QWEN38_FLASH_NEXT_MTP_COSTS_HPP_
#define GUFO_MODELS_QWEN38_FLASH_NEXT_MTP_COSTS_HPP_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace gufo::models::qwen38_flash_next {

// gfx1151/LLVM23, Flash-Next UD-Q4_K_XL target and matched Q4_K/Q5_1 MTP,
// 2026-09-26. Milliseconds per
// cohort, including catch-up, recursive proposals and target verification.
// Rows are widths 1..8 (width 1 is headless MTP catch-up plus ordinary decode).
// Reproduce with qwen38_flash_next_gpu_probe --cost-audit 0. Allocation and C1
// graph capture are warmed outside the measurement; each row is the median
// of three complete cycles. Use --depth N to audit one depth. See benchmarks.
inline constexpr float kMtpCycleMilliseconds[3][5][8] = {
    {
        // Context 0
        {39.4302F, 48.9467F, 57.2983F, 66.6781F, 77.4997F, 84.8979F, 94.2593F, 101.9392F},  // C1
        {45.6992F, 61.4905F, 72.5118F, 88.4838F, 104.5959F, 116.3422F, 131.8782F, 140.3606F},  // C2
        {57.4744F, 80.3411F, 104.1202F, 125.6680F, 151.4823F, 170.3378F, 198.9462F, 218.3609F},  // C4
        {69.1252F, 105.6764F, 134.2734F, 164.9293F, 196.3050F, 232.9849F, 266.3281F, 297.7349F},  // C6
        {79.5861F, 121.9452F, 160.0478F, 204.8834F, 251.0340F, 283.6211F, 335.1608F, 369.5175F},  // C8
    },
    {
        // Context 4096
        {38.7969F, 49.1435F, 60.8334F, 68.1366F, 79.8503F, 86.7584F, 96.4505F, 105.2551F},  // C1
        {48.4193F, 64.6565F, 78.2549F, 93.2866F, 111.1826F, 122.0169F, 136.1543F, 147.9875F},  // C2
        {63.5396F, 87.3510F, 113.4239F, 135.1234F, 163.9223F, 182.6662F, 212.3619F, 232.7220F},  // C4
        {74.2631F, 116.0111F, 150.7770F, 181.8591F, 215.9367F, 254.8115F, 294.6950F, 323.0494F},  // C6
        {88.6198F, 133.2655F, 177.7988F, 227.0071F, 283.1865F, 314.6648F, 366.2626F, 405.8709F},  // C8
    },
    {
        // Context 32768
        {41.9774F, 51.7758F, 60.9657F, 71.6190F, 81.2277F, 89.7823F, 100.4534F, 108.5828F},  // C1
        {54.8742F, 69.1898F, 83.6116F, 97.2405F, 114.6304F, 128.5177F, 144.5766F, 155.6788F},  // C2
        {73.3965F, 95.2736F, 119.7701F, 142.7904F, 171.0958F, 193.9585F, 225.3005F, 243.3250F},  // C4
        {88.9523F, 124.8655F, 157.8300F, 195.3243F, 229.3220F, 273.2681F, 312.7557F, 344.4363F},  // C6
        {105.8625F, 149.4559F, 189.9829F, 238.9349F, 292.2113F, 339.2317F, 393.3424F, 431.7243F},  // C8
    },
};

// Sampled policy uses fixed configured capacity for reproducibility; greedy
// batch policy uses these curves to bootstrap each physical occupancy.
// Intermediate capacities use the next measured cohort. Context costs
// interpolate, then extrapolate the measured QSA slope to the native limit.
[[nodiscard]] inline std::array<float, 8> MtpCycleCosts(
    std::uint32_t context, std::uint32_t concurrency) noexcept {
  const auto cohort = concurrency <= 1   ? 0
                      : concurrency <= 2 ? 1
                      : concurrency <= 4 ? 2
                      : concurrency <= 6 ? 3
                                         : 4;
  const auto interval = context <= 4096 ? 0 : 1;
  const float fraction =
      interval == 0
          ? static_cast<float>(context) / 4096.0F
          : static_cast<float>(std::min(context, 262144U) - 4096) / 28672.0F;
  std::array<float, 8> costs{};
  for (std::size_t i = 0; i < costs.size(); ++i) {
    const auto low = kMtpCycleMilliseconds[interval][cohort][i];
    const auto high = kMtpCycleMilliseconds[interval + 1][cohort][i];
    costs[i] = low + fraction * std::max(high - low, 0.0F);
    if (fraction > 1.0F && i != 0) {
      // Independently extrapolated noisy slopes can cross at long contexts.
      // Preserve at least the last measured marginal cost of another draft.
      const auto marginal =
          high - kMtpCycleMilliseconds[interval + 1][cohort][i - 1];
      costs[i] = std::max(costs[i], costs[i - 1] + marginal);
    }
  }
  return costs;
}

}  // namespace gufo::models::qwen38_flash_next

#endif  // GUFO_MODELS_QWEN38_FLASH_NEXT_MTP_COSTS_HPP_
