#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "generation_profile_guard.hpp"

using namespace std::chrono_literals;

static std::atomic<int> resets{0};
static void ResetExtra() { ++resets; }
static void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    std::mutex generation_mutex;
    std::promise<void> queued, entered, release;
    auto queued_future = queued.get_future();
    auto entered_future = entered.get_future();
    auto release_future = release.get_future();
    std::jthread second;
    {
      fnvprof::GenerationProfileGuard first(generation_mutex, true, ResetExtra);
      fnvprof::SetPhase(fnvprof::Phase::Decode);
      fnvprof::S().graph_captures = 123;
      second = std::jthread([&] {
        queued.set_value();
        fnvprof::GenerationProfileGuard next(generation_mutex, true, ResetExtra);
        entered.set_value();
        release_future.wait();
      });
      queued_future.wait();
      const bool blocked = entered_future.wait_for(100ms) == std::future_status::timeout;
      const bool untouched = resets.load() == 1 && fnvprof::Enabled() &&
          fnvprof::CurrentPhase() == fnvprof::Phase::Decode &&
          fnvprof::S().graph_captures == 123;
      // Release even on failure so a regression cannot hang the test runner.
      if (!blocked || !untouched) release.set_value();
      Check(blocked, "queued request entered while first request owned generation");
      Check(untouched, "queued request changed active profiler state");
    }
    entered_future.wait();
    const bool initialized = resets.load() == 2 && fnvprof::Enabled() &&
        fnvprof::CurrentPhase() == fnvprof::Phase::Prefill &&
        fnvprof::S().graph_captures == 0;
    release.set_value();
    second.join();
    Check(initialized, "first request teardown disabled the next profiler");
    Check(!fnvprof::Enabled() && fnvprof::CurrentPhase() == fnvprof::Phase::Off,
          "profiler remained enabled after request completion");
    {
      fnvprof::GenerationProfileGuard unprofiled(generation_mutex, false, ResetExtra);
      Check(resets.load() == 2 && !fnvprof::Enabled(),
            "unprofiled request reset or enabled profiling");
      auto peer = std::async(std::launch::async, [&] {
        const bool acquired = generation_mutex.try_lock();
        if (acquired) generation_mutex.unlock();
        return acquired;
      });
      Check(!peer.get(), "unprofiled request did not own generation");
    }
    Check(generation_mutex.try_lock(), "generation lock was not released");
    generation_mutex.unlock();
    std::cout << "PASS: two-thread profiler ownership and teardown isolation\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
