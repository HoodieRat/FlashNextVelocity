#pragma once

#include <mutex>

#include "bench_profile.hpp"

namespace fnvprof {

// Both profiling state and inference scratch belong to the request holding
// the generation lock. Keep initialization and teardown inside that lifetime.
class GenerationProfileGuard {
 public:
  GenerationProfileGuard(std::mutex& generation_mutex, bool enabled,
                         void (*reset_extra)())
      : lock_(generation_mutex), enabled_(enabled) {
    if (!enabled_) return;
    Reset();
    if (reset_extra != nullptr) reset_extra();
    SetEnabled(true);
    SetPhase(Phase::Prefill);
  }

  ~GenerationProfileGuard() {
    if (!enabled_) return;
    SetPhase(Phase::Off);
    SetEnabled(false);
  }

  GenerationProfileGuard(const GenerationProfileGuard&) = delete;
  GenerationProfileGuard& operator=(const GenerationProfileGuard&) = delete;

 private:
  std::unique_lock<std::mutex> lock_;
  bool enabled_;
};

}  // namespace fnvprof
