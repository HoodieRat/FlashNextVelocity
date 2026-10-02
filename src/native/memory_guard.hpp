#pragma once

#include <algorithm>
#include <cstdint>

// Startup memory decision. This engine is the gfx1151 build. A large Strix
// Halo UMA carve-out leaves a small Windows-visible physical pool while commit
// headroom stays large; that case uses a lower physical floor.
struct WindowsMemoryGuardSample {
  std::uint64_t available_physical{0};
  std::uint64_t total_physical{0};
  std::uint64_t available_commit{0};
  double configured_floor_gib{0};
  bool strix_halo{false};
};

struct WindowsMemoryGuardDecision {
  bool allow{false};
  bool large_uma{false};
  double physical_floor_gib{0};
  double commit_floor_gib{0};
};

inline constexpr double kStrixHaloUmaPhysicalFloorGib = 8.0;
inline constexpr double kStrixHaloVisiblePoolMaxGib = 48.0;

[[nodiscard]] inline WindowsMemoryGuardDecision DecideWindowsMemoryGuard(
    const WindowsMemoryGuardSample& sample) noexcept {
  constexpr double kGiB = static_cast<double>(1ULL << 30);
  const double total_gib =
      sample.total_physical == 0
          ? 0.0
          : static_cast<double>(sample.total_physical) / kGiB;
  const double commit_gib =
      sample.available_commit == 0
          ? 0.0
          : static_cast<double>(sample.available_commit) / kGiB;
  const double configured = sample.configured_floor_gib;
  const bool large_uma = sample.strix_halo && total_gib > 0.0 &&
                         total_gib <= kStrixHaloVisiblePoolMaxGib &&
                         commit_gib >= configured;
  const double physical_floor =
      large_uma ? std::min(configured, kStrixHaloUmaPhysicalFloorGib)
                : configured;
  const auto floor_bytes = [&](double gib) {
    return static_cast<std::uint64_t>(gib * kGiB);
  };
  const bool physical_low =
      sample.available_physical != 0 &&
      sample.available_physical < floor_bytes(physical_floor);
  const bool commit_low = sample.available_commit != 0 &&
                          sample.available_commit < floor_bytes(configured);
  return {!physical_low && !commit_low, large_uma, physical_floor, configured};
}
