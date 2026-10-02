#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "memory_guard.hpp"
#include "src/models/qwen38_flash_next/context_lookup.hpp"
#include "src/models/qwen38_flash_next/mtp_sampling.hpp"
#include "src/models/qwen38_flash_next/mtp_policy.hpp"
#include <cmath>
#include "bench_profile.hpp"

namespace qfn = gufo::models::qwen38_flash_next;

static void Check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

#include "chat_output_tests.hpp"

static void TestContextLookup() {
  {
    const std::vector<std::int32_t> history{1, 2, 3, 4, 5, 6, 9, 1, 2, 3};
    const auto match = qfn::FindContextLookup(history, 4, 3, 3, 6, 32768, 2);
    Check(match.has_value(), "expected four-token lookup match");
    Check(match->ngram == 4, "wrong lookup n-gram order");
    Check(match->continuation == std::vector<std::int32_t>({5, 6, 9}),
          "wrong lookup continuation");
    Check(match->source_end == 3, "wrong lookup source position");
    Check(match->distance == 6, "wrong lookup source distance");
  }
  {
    // Search newest-first when identical n-grams have different continuations.
    const std::vector<std::int32_t> history{
        7, 8, 9, 4, 10, 11, 0, 7, 8, 9, 4, 20, 21, 0, 7, 8, 9};
    const auto match = qfn::FindContextLookup(history, 4, 2, 4, 4, 0, 2);
    Check(match.has_value(), "expected newest lookup match");
    Check(match->source_end == 10, "lookup did not choose newest match");
    Check(match->continuation == std::vector<std::int32_t>({20, 21}),
          "newest match continuation is wrong");
  }
  {
    // Tiny histories must fail safely without unsigned-index underflow.
    const std::vector<std::int32_t> history{7};
    Check(!qfn::FindContextLookup(history, 6, 2, 2, 5, 0, 1),
          "tiny-history lookup should not produce a match");
  }
  {
    // A bounded search must not leak beyond the configured history window.
    const std::vector<std::int32_t> history{1, 2, 3, 4, 5, 6, 7, 8, 9, 1, 2, 3};
    Check(!qfn::FindContextLookup(history, 4, 2, 4, 4, 5, 2),
          "lookup escaped configured search window");
  }
  {
    // Reject a candidate that cannot supply the requested minimum draft.
    const std::vector<std::int32_t> history{1, 4, 1};
    Check(!qfn::FindContextLookup(history, 4, 3, 2, 2, 0, 2),
          "lookup ignored minimum draft length");
  }
  {
    // Fall back from an unavailable longer n-gram to a valid shorter one.
    const std::vector<std::int32_t> history{8, 2, 3, 4, 5, 6, 7, 9, 2, 3};
    const auto match = qfn::FindContextLookup(history, 4, 2, 3, 5, 0, 2);
    Check(match.has_value(), "expected shorter n-gram fallback");
    Check(match->ngram == 3, "wrong fallback n-gram order");
    Check(match->continuation == std::vector<std::int32_t>({5, 6}),
          "wrong fallback continuation");
  }
  {
    // Lookup may copy a longer committed continuation than MTP can draft.
    std::vector<std::int32_t> history{7, 8, 9, 4};
    for (std::int32_t token = 100; token < 120; ++token)
      history.push_back(token);
    history.insert(history.end(), {7, 8, 9});
    const auto match = qfn::FindContextLookup(history, 4, 16, 4, 6, 0, 6);
    Check(match.has_value(), "expected long lookup match");
    Check(match->continuation.size() == 16,
          "lookup did not use its independent maximum width");
    for (std::int32_t i = 0; i < 16; ++i)
      Check(match->continuation[i] == 100 + i,
            "lookup continuation differs from committed context");
  }
}


static void TestHaloGreedyProposal() {
  qfn::MtpCandidateLogits candidates;
  candidates.size = 3;
  candidates.ids[0] = 42; candidates.logits[0] = 3.0F;
  candidates.ids[1] = 7;  candidates.logits[1] = 4.0F;
  candidates.ids[2] = 99; candidates.logits[2] = 2.0F;
  const auto proposal = qfn::HaloGreedyMtpProposal(candidates);
  Check(proposal.token == 7, "halo-greedy proposal did not select raw MTP argmax");
  Check(proposal.size == 1 && proposal.ids[0] == 7 &&
            proposal.probability == 1.0F && proposal.probabilities[0] == 1.0F,
        "halo-greedy proposal is not deterministic one-hot q");
  const double expected = 1.0 / (1.0 + std::exp(-1.0) + std::exp(-2.0));
  Check(std::abs(proposal.confidence - expected) < 1e-6,
        "halo-greedy Top-10 confidence is wrong");

  // Tie behavior is deterministic and independent of candidate input order.
  candidates.size = 2;
  candidates.ids[0] = 50; candidates.logits[0] = 5.0F;
  candidates.ids[1] = 12; candidates.logits[1] = 5.0F;
  Check(qfn::HaloGreedyMtpProposal(candidates).token == 12,
        "halo-greedy argmax tie break is not stable");
}


static void TestDeferredFrontierTelemetry() {
  fnvprof::Reset();
  fnvprof::SetEnabled(true);
  fnvprof::SetPhase(fnvprof::Phase::Decode);

  // A successful compact verification cycle transfers only the compact
  // shortlist. The full target row is deferred until an exact fallback needs it.
  fnvprof::AddVerificationTransfer(0, 4, 8192, 1024);
  fnvprof::NoteCertificateSuccess();
  auto report = fnvprof::BuildReport(1.0);
  Check(report.verify_full_rows_d2h == 0 &&
            report.verify_compact_rows_d2h == 4 &&
            report.verify_d2h_bytes == 8192 &&
            report.verify_compact_candidates == 1024,
        "compact verification telemetry still charges an eager full row");
  Check(report.verify_certificate_successes == 1 &&
            report.verify_certificate_fallbacks == 0,
        "compact certificate success telemetry is wrong");

  // If the certificate later fails, the exact full row is charged exactly at
  // fallback time rather than on every compact cycle.
  fnvprof::NoteCertificateFallback();
  fnvprof::AddVerificationTransfer(1, 0, 1000000, 0);
  report = fnvprof::BuildReport(1.0);
  Check(report.verify_full_rows_d2h == 1 &&
            report.verify_compact_rows_d2h == 4 &&
            report.verify_d2h_bytes == 1008192,
        "deferred full-row fallback telemetry is wrong");
  Check(report.verify_certificate_successes == 1 &&
            report.verify_certificate_fallbacks == 1,
        "compact certificate fallback telemetry is wrong");
}

static void TestProposalSourceTelemetry() {
  fnvprof::Reset();
  fnvprof::SetEnabled(true);
  fnvprof::SetPhase(fnvprof::Phase::Decode);

  fnvprof::BeginCycle();
  fnvprof::EndCycle(2, 2, 1, 2, fnvprof::ProposalSource::Mtp);
  fnvprof::BeginCycle();
  fnvprof::EndCycle(3, 3, 3, 4, fnvprof::ProposalSource::Lookup);

  const auto report = fnvprof::BuildReport(1.0);
  Check(report.mtp_cycles == 1 && report.lookup_cycles == 1,
        "proposal-source cycle split is wrong");
  Check(report.mtp_drafted == 2 && report.mtp_accepted == 1,
        "MTP counters are wrong");
  Check(report.lookup_drafted == 3 && report.lookup_accepted == 3,
        "lookup counters are wrong");
  Check(report.mtp_output_tokens == 2 && report.lookup_output_tokens == 4,
        "source output counters are wrong");
  Check(report.mtp_depths[2].cycles == 1 && report.lookup_depths[3].cycles == 1,
        "source depth rows are wrong");
  Check(report.mtp_proposed_per_position[0] == 1 &&
            report.mtp_accepted_per_position[0] == 1 &&
            report.mtp_proposed_per_position[1] == 1 &&
            report.mtp_accepted_per_position[1] == 0,
        "MTP prefix survival counters are wrong");
  Check(report.lookup_proposed_per_position[2] == 1 &&
            report.lookup_accepted_per_position[2] == 1,
        "lookup prefix survival counters are wrong");

  fnvprof::Reset();
  const auto reset = fnvprof::BuildReport(1.0);
  Check(reset.mtp_depths[2].cycles == 0 && reset.lookup_depths[3].cycles == 0,
        "profiling reset left source depth counters dirty");
}

// Host mirror of the device rollback contract. Path A is ordinary
// autoregressive execution of the accepted prefix. Path B fills the
// speculative ring the way RollingSnapshotKernel / SsmConv4Kernel /
// HashNgramRows do, then restores slot keep-1, which is what
// Executor::Rollback uses.
static void ShiftHistoryOne(std::vector<float>* history, std::uint32_t channels,
                            const float* row) {
  const std::size_t row_n = channels;
  if (history->size() <= row_n) {
    std::copy(row, row + row_n, history->begin());
    return;
  }
  std::copy(history->begin() + row_n, history->end(), history->begin());
  std::copy(row, row + row_n, history->end() - row_n);
}

static std::vector<float> HistoryAfterPrefix(const std::vector<float>& initial,
                                            const std::vector<float>& rows,
                                            std::uint32_t channels,
                                            std::uint32_t count) {
  std::vector<float> history = initial;
  for (std::uint32_t t = 0; t < count; ++t) {
    ShiftHistoryOne(&history, channels, rows.data() + static_cast<std::size_t>(t) * channels);
  }
  return history;
}

// RollingSnapshotKernel: row j after token t is concat index (t + 1 + j).
static std::vector<float> SnapshotAfterToken(const std::vector<float>& initial,
                                             const std::vector<float>& rows,
                                             std::uint32_t channels,
                                             std::uint32_t hist_rows,
                                             std::uint32_t token) {
  std::vector<float> snap(static_cast<std::size_t>(hist_rows) * channels);
  for (std::uint32_t j = 0; j < hist_rows; ++j) {
    const std::uint32_t src = token + 1 + j;
    const float* from =
        src < hist_rows
            ? initial.data() + static_cast<std::size_t>(src) * channels
            : rows.data() + static_cast<std::size_t>(src - hist_rows) * channels;
    std::copy(from, from + channels,
              snap.begin() + static_cast<std::size_t>(j) * channels);
  }
  return snap;
}

static std::vector<float> Conv4WindowAfterToken(const std::vector<float>& initial,
                                                const std::vector<float>& rows,
                                                std::uint32_t channels,
                                                std::uint32_t token) {
  std::vector<float> window = initial;
  for (std::uint32_t t = 0; t <= token; ++t) {
    std::copy(window.begin() + channels, window.end(), window.begin());
    std::copy(rows.data() + static_cast<std::size_t>(t) * channels,
              rows.data() + static_cast<std::size_t>(t + 1) * channels,
              window.end() - channels);
  }
  return window;
}

static void TestRollbackMatchesAutoregressivePrefix() {
  const std::uint32_t channels = 4;
  for (std::uint32_t hist_rows : {1U, 2U, 3U, 4U, 6U}) {
    for (std::uint32_t batch = 2; batch <= 7; ++batch) {
      std::vector<float> initial(static_cast<std::size_t>(hist_rows) * channels);
      std::vector<float> rows(static_cast<std::size_t>(batch) * channels);
      for (std::size_t i = 0; i < initial.size(); ++i)
        initial[i] = static_cast<float>(1000 + i);
      for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i] = static_cast<float>(5000 + i);
      std::vector<std::vector<float>> ring(batch - 1);
      for (std::uint32_t t = 0; t + 1 < batch; ++t) {
        ring[t] = SnapshotAfterToken(initial, rows, channels, hist_rows, t);
      }
      for (std::uint32_t keep = 1; keep < batch; ++keep) {
        const auto ar =
            HistoryAfterPrefix(initial, rows, channels, keep);
        const auto& restored = ring[keep - 1];
        Check(restored == ar,
              "PLE/GDN-conv rollback slot does not match autoregressive history");
        if (keep + 1 < batch) {
          Check(ring[keep] != ar,
                "a later ring slot accidentally matched the accepted prefix");
        }
        if (keep >= 2) {
          Check(ring[keep - 2] != ar,
                "the previous ring slot accidentally matched the accepted prefix");
        }
      }
      if (hist_rows == 3) {
        for (std::uint32_t t = 0; t + 1 < batch; ++t) {
          const auto window = Conv4WindowAfterToken(initial, rows, channels, t);
          Check(window == ring[t],
                "SsmConv4 history snapshot does not match the rollback ring");
        }
      }
    }
  }

  // PLE conv tap: dilation reads history[hist - back], matching the reference.
  const std::uint32_t kernel = 4;
  const std::uint32_t dilation = 2;
  const std::uint32_t hist = (kernel - 1) * dilation;
  for (std::uint32_t k = 0; k < kernel; ++k) {
    const std::uint32_t back = (kernel - 1 - k) * dilation;
    const std::int32_t src_t = -static_cast<std::int32_t>(back);
    const std::uint32_t device_row =
        back == 0 ? 0 : hist + static_cast<std::uint32_t>(src_t);
    const std::uint32_t reference_row = back == 0 ? 0 : hist - back;
    Check(back == 0 || device_row == reference_row,
          "PLE dilated history tap does not match the reference row");
  }

  // N-gram previous-token ring. Snapshots are taken after each token except
  // the last, and rollback restores slot keep-1.
  constexpr int kPrev = 6;
  std::array<std::int32_t, kPrev> initial_prev{};
  for (int i = 0; i < kPrev; ++i) initial_prev[i] = 10 + i;
  const std::array<std::int32_t, 5> speculated{{41, 42, 43, 44, 45}};
  auto push = [](std::array<std::int32_t, kPrev>* prev, std::int32_t token) {
    for (int s = kPrev; s-- > 1;) (*prev)[s] = (*prev)[s - 1];
    (*prev)[0] = token;
  };
  std::array<std::array<std::int32_t, kPrev>, 4> ngram_ring{};
  auto cursor = initial_prev;
  for (std::uint32_t i = 0; i < speculated.size(); ++i) {
    push(&cursor, speculated[i]);
    if (i + 1 < speculated.size()) ngram_ring[i] = cursor;
  }
  for (std::uint32_t keep = 1; keep < speculated.size(); ++keep) {
    auto ar = initial_prev;
    for (std::uint32_t i = 0; i < keep; ++i) push(&ar, speculated[i]);
    Check(ngram_ring[keep - 1] == ar,
          "n-gram rollback slot does not match autoregressive history");
  }

  // Recurrent state: slot 0 is the state after token 0. Later slots store the
  // update applied by that token. Restoring keep replays updates [1, keep).
  auto apply = [](float state, float decay, float beta, float error, float key) {
    const float correction = error * key;
    return beta * correction + state * decay;
  };
  const std::array<std::array<float, 4>, 4> factors{{
      {{0.90F, 0.20F, 0.50F, 1.25F}},
      {{0.80F, 0.30F, -0.40F, 0.75F}},
      {{0.70F, 0.10F, 0.25F, -1.50F}},
      {{0.60F, 0.40F, 1.00F, 0.30F}},
  }};
  float live = 3.5F;
  std::array<float, 4> after{};
  for (std::uint32_t t = 0; t < factors.size(); ++t) {
    live = apply(live, factors[t][0], factors[t][1], factors[t][2], factors[t][3]);
    after[t] = live;
  }
  for (std::uint32_t keep = 1; keep <= factors.size(); ++keep) {
    float restored = after[0];
    for (std::uint32_t t = 1; t < keep; ++t) {
      restored = apply(restored, factors[t][0], factors[t][1], factors[t][2],
                       factors[t][3]);
    }
    float ar = 3.5F;
    for (std::uint32_t t = 0; t < keep; ++t) {
      ar = apply(ar, factors[t][0], factors[t][1], factors[t][2], factors[t][3]);
    }
    Check(restored == ar,
          "GDN rollback replay does not match autoregressive state");
    Check(restored == after[keep - 1],
          "GDN rollback did not land on the accepted token's state");
  }

  // Accepted boundary: trunk position and KV length stay at spec_base + keep.
  // MTP is rewound to the pre-batch position so the next catch-up replays the
  // accepted suffix, not the rejected tail.
  const std::uint32_t spec_base = 10;
  const std::array<std::int32_t, 4> batch{{7, 8, 9, 11}};
  for (std::uint32_t keep = 1; keep < batch.size(); ++keep) {
    std::vector<std::int32_t> committed(batch.begin(), batch.begin() + keep);
    const std::uint32_t position = spec_base + keep;
    Check(position == spec_base + static_cast<std::uint32_t>(committed.size()),
          "rollback position dropped or extended the accepted prefix");
    const std::uint32_t mtp = spec_base;
    std::vector<std::int32_t> replay(committed.begin() + 1, committed.end());
    const std::int32_t next = 99;
    replay.push_back(next);
    std::vector<std::int32_t> expected(batch.begin() + 1, batch.begin() + keep);
    expected.push_back(next);
    Check(mtp + 1 == spec_base + 1, "MTP rewind left the pre-batch position");
    Check(replay == expected,
          "MTP catch-up after rollback does not replay the accepted suffix");
    Check(replay.front() != batch[0] || keep == 1,
          "MTP catch-up repeated the anchor already consumed before this batch");
  }
}

static std::uint64_t GiB(double n) {
  return static_cast<std::uint64_t>(n * static_cast<double>(1ULL << 30));
}

static void TestMemoryGuard() {
  const WindowsMemoryGuardSample strix{
      GiB(12.30), GiB(31.65), GiB(127.06), 20.0, true};
  const auto uma = DecideWindowsMemoryGuard(strix);
  Check(uma.large_uma, "Strix Halo UMA mode was not selected");
  Check(uma.physical_floor_gib == 8.0, "UMA physical floor is not 8 GiB");
  Check(uma.commit_floor_gib == 20.0, "commit floor changed");
  Check(uma.allow, "valid Strix Halo UMA startup was rejected");

  const auto normal = DecideWindowsMemoryGuard(
      {GiB(12.30), GiB(128.0), GiB(127.06), 20.0, false});
  Check(!normal.large_uma && !normal.allow,
        "a normal system under the configured floor was allowed");

  const auto full_pool = DecideWindowsMemoryGuard(
      {GiB(12.30), GiB(120.0), GiB(127.06), 20.0, true});
  Check(!full_pool.large_uma && !full_pool.allow,
        "gfx1151 with a full physical pool ignored the configured floor");

  const auto low_phys = DecideWindowsMemoryGuard(
      {GiB(5.0), GiB(31.65), GiB(127.06), 20.0, true});
  Check(low_phys.large_uma && !low_phys.allow,
        "physical memory below 8 GiB was allowed");

  const auto low_commit = DecideWindowsMemoryGuard(
      {GiB(12.30), GiB(31.65), GiB(10.0), 20.0, true});
  Check(!low_commit.allow, "low commit headroom was allowed");

  const auto below_configured = DecideWindowsMemoryGuard(
      {GiB(5.0), GiB(31.65), GiB(40.0), 4.0, true});
  Check(below_configured.allow && below_configured.physical_floor_gib == 4.0,
        "a configured floor under 8 GiB was raised");
}

static void TestLookupPolicy() {
  qfn::ContextLookupPolicy p;
  p.Reset(qfn::ContextLookupMode::kSticky);
  p.Observe(6, 5, 6);
  p.Observe(5, 5, 12);
  p.Observe(6, 6, 19);
  Check(p.active_width == 6, "uncapped proposal supplied promotion evidence");
  p.Observe(6, 4, 24);
  p.Observe(6, 5, 30);
  Check(p.active_width == 6, "weak sample did not reset promotion evidence");
  p.Observe(6, 5, 36);
  Check(p.active_width == 16 && p.promotions == 1 && p.promotion_round == 6 &&
        p.promotion_token == 36, "two capped 5/6 samples did not promote");
  p.Observe(16, 4, 41);
  p.Observe(16, 0, 42);
  p.Observe(16, 7, 50);
  p.Observe(16, 5, 56);
  p.Observe(5, 0, 57);
  Check(p.active_width == 16 && p.promotions == 1 && p.promotion_token == 36,
        "weak wide spans changed sticky promotion");
  Check(p.rounds[0] == 6 && p.rounds[1] == 5 && p.capped[0] == 5 &&
        p.capped[1] == 4 && p.proposed[1] == 69 && p.accepted[1] == 16,
        "lookup width telemetry is wrong");
  p.Reset(qfn::ContextLookupMode::kSticky);
  Check(p.active_width == 6 && p.starting_width == 6 && !p.rounds[0] &&
        !p.rounds[1] && !p.promotions && !p.strong && !p.promotion_token,
        "request reset inherited sticky evidence");
  p.Observe(6, 6, 7);
  Check(p.active_width == 6, "request reset inherited promotion streak");
  p.Observe(6, 6, 14);
  Check(p.active_width == 16 && p.promotion_round == 2,
        "next request could not independently promote");
  for (auto mode : {qfn::ContextLookupMode::kFixed6, qfn::ContextLookupMode::kFixed16}) {
    p.Reset(mode);
    for (unsigned i = 0; i < 4; ++i) p.Observe(p.active_width, 6, i * 7);
    p.Observe(p.active_width, 0, 29);
    Check(p.active_width == p.starting_width && !p.promotions,
          "fixed policy changed width");
  }
}

static void TestDistributionQ8C1Costs() {
  qfn::DistributionQ8C1ProfileInputs p{
      true,true,true,true,1,32768,4096,7,17,
      "Qwen3.8-Flash-Next-UD-Q4_K_XL-00001-of-00004.gguf",
      "mtp-Qwen3.8-Flash-Next-shared-Q8_0.gguf"};
  Check(qfn::MatchesDistributionQ8C1Profile(p), "measured profile did not match");
  const auto reject=[&](auto change) {
    auto other=p;change(other);
    Check(!qfn::MatchesDistributionQ8C1Profile(other), "unmeasured profile matched");
  };
  reject([](auto& v){v.distribution=false;});
  reject([](auto& v){v.latin=false;});
  reject([](auto& v){v.q8_sidecar=false;});
  reject([](auto& v){v.lookup_enabled=false;});
  reject([](auto& v){v.concurrency=2;});
  reject([](auto& v){v.context=4096;});
  reject([](auto& v){v.prefill_batch=2432;});
  reject([](auto& v){v.max_drafts=6;});
  reject([](auto& v){v.verification_capacity=8;});
  reject([](auto& v){v.target_basename="another.gguf";});
  reject([](auto& v){v.sidecar_basename="another.gguf";});
  const auto cold=qfn::DistributionQ8C1CycleCosts(0);
  const auto middle=qfn::DistributionQ8C1CycleCosts(2048);
  for (unsigned i=0;i<8;++i) {
    const auto expected=cold[i]+.5F*std::max(
        qfn::kDistributionQ8C1CycleMilliseconds[1][i]-cold[i],0.F);
    Check(std::abs(middle[i]-expected)<.0001F, "cost interpolation changed");
  }
  Check(qfn::MtpProfileCycleCosts(qfn::MtpCostProfile::kIncumbent,16000,1)==
        qfn::MtpCycleCosts(16000,1), "incumbent costs changed");
  Check(qfn::MtpProfileCycleCosts(qfn::MtpCostProfile::kDistributionQ8C1,16000,2)==
        qfn::MtpCycleCosts(16000,2), "unmeasured capacity used C1 costs");
  qfn::MtpLengthController policy(7,1);
  policy.Observe(1,3,1000);
  const auto before=policy.State();
  policy.SetCostProfile(qfn::MtpCostProfile::kDistributionQ8C1);
  const auto after=policy.State();
  Check(before.successes==after.successes && before.failures==after.failures &&
        before.retry_tokens==after.retry_tokens && before.probe_depth==after.probe_depth &&
        before.explored_depth==after.explored_depth && before.probe_delay==after.probe_delay &&
        before.failed_depths==after.failed_depths, "cost switch reset acceptance state");
}

int main() {
  try {
    TestChatOutput();
    TestDistributionQ8C1Costs();
    TestMemoryGuard();
    TestLookupPolicy();
    TestContextLookup();
    TestHaloGreedyProposal();
    TestDeferredFrontierTelemetry();
    TestProposalSourceTelemetry();
    TestRollbackMatchesAutoregressivePrefix();
    std::cout << "FlashNextVelocity native unit tests passed.\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "FlashNextVelocity native unit tests FAILED: " << ex.what()
              << "\n";
    return 1;
  }
}
