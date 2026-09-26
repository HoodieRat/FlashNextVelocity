#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "src/models/qwen38_flash_next/context_lookup.hpp"
#include "src/models/qwen38_flash_next/mtp_sampling.hpp"
#include <cmath>
#include "bench_profile.hpp"

namespace qfn = gufo::models::qwen38_flash_next;

static void Check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

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

int main() {
  try {
    TestContextLookup();
    TestHaloGreedyProposal();
    TestDeferredFrontierTelemetry();
    TestProposalSourceTelemetry();
    std::cout << "FlashNextVelocity native unit tests passed.\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "FlashNextVelocity native unit tests FAILED: " << ex.what()
              << "\n";
    return 1;
  }
}
