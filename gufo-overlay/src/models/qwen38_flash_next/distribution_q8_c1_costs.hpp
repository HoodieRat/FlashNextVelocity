#ifndef GUFO_MODELS_QWEN38_FLASH_NEXT_DISTRIBUTION_Q8_C1_COSTS_HPP_
#define GUFO_MODELS_QWEN38_FLASH_NEXT_DISTRIBUTION_Q8_C1_COSTS_HPP_

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include "src/models/qwen38_flash_next/mtp_costs.hpp"

namespace gufo::models::qwen38_flash_next {

enum class MtpCostProfile : std::uint32_t { kIncumbent, kDistributionQ8C1 };

struct DistributionQ8C1ProfileInputs {
  bool distribution;
  bool latin;
  bool q8_sidecar;
  bool lookup_enabled;
  std::uint32_t concurrency;
  std::uint32_t context;
  std::uint32_t prefill_batch;
  std::uint32_t max_drafts;
  std::uint32_t verification_capacity;
  std::string_view target_basename;
  std::string_view sidecar_basename;
};

[[nodiscard]] inline constexpr bool MatchesDistributionQ8C1Profile(
    const DistributionQ8C1ProfileInputs& p) noexcept {
  return p.distribution && p.latin && p.q8_sidecar && p.lookup_enabled &&
      p.concurrency == 1 && p.context == 32768 && p.prefill_batch == 4096 &&
      p.max_drafts == 7 && p.verification_capacity == 17 &&
      p.target_basename == "Qwen3.8-Flash-Next-UD-Q4_K_XL-00001-of-00004.gguf" &&
      p.sidecar_basename == "mtp-Qwen3.8-Flash-Next-shared-Q8_0.gguf";
}

// Windows gfx1151, 2026-10-01. Current Latin Q8 distribution C1 geometry.
// Warmed complete fixed-width cycles include preparation, known-hidden
// catch-up, sampled proposals, exact CPU verification, rollback and the next
// frontier draw. Two warmups then median of three; confidence is evaluated by
// live serving, not imposed on these physical widths. Source:
// .cache/mtp-cost-audit-complete.log. Anchor convention matches incumbent.
inline constexpr float kDistributionQ8C1CycleMilliseconds[3][8] = {
    {39.7067F,53.3004F,57.3727F,64.1296F,75.4523F,82.2367F,89.6888F,97.3212F},
    {40.7353F,46.9636F,55.2885F,65.3428F,72.9965F,82.2127F,88.4364F,96.1453F},
    {42.4606F,50.8812F,59.3088F,67.4347F,79.5476F,86.5691F,95.0872F,104.4875F},
};

[[nodiscard]] inline std::array<float,8> DistributionQ8C1CycleCosts(
    std::uint32_t context) noexcept {
  const auto interval = context <= 4096 ? 0 : 1;
  const float fraction = interval == 0
      ? static_cast<float>(context) / 4096.0F
      : static_cast<float>(std::min(context,262144U)-4096) / 28672.0F;
  std::array<float,8> costs{};
  for (std::size_t i=0;i<costs.size();++i) {
    const auto low=kDistributionQ8C1CycleMilliseconds[interval][i];
    const auto high=kDistributionQ8C1CycleMilliseconds[interval+1][i];
    // Preserve the interpolation used in the accepted experiment, including
    // its suppression of a negative measured context slope.
    costs[i]=low+fraction*std::max(high-low,0.0F);
    if (fraction>1.0F && i!=0) {
      const auto marginal=high-kDistributionQ8C1CycleMilliseconds[interval+1][i-1];
      costs[i]=std::max(costs[i],costs[i-1]+marginal);
    }
  }
  return costs;
}

[[nodiscard]] inline std::array<float,8> MtpProfileCycleCosts(
    MtpCostProfile profile,std::uint32_t context,std::uint32_t concurrency) noexcept {
  return profile==MtpCostProfile::kDistributionQ8C1 && concurrency==1
      ? DistributionQ8C1CycleCosts(context) : MtpCycleCosts(context,concurrency);
}

} // namespace gufo::models::qwen38_flash_next
#endif
