// Adapted from pinned Gufo's warmed complete-cycle cost audit for the actual
// Windows C1 distribution/Latin Q8 MTP path. Fixed widths isolate physical
// costs; acceptance and confidence economics remain in the serving controller.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
#include "src/models/qwen/tokenizer.hpp"
#include "src/models/qwen38_flash_next/kernels/rocm/executor.hpp"
#include "src/models/qwen38_flash_next/mtp_sampling.hpp"
namespace q=gufo::models::qwen38_flash_next;
static void Require(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
void AuditMtpCosts(q::rocm::Executor& exec,
                   const gufo::tokenization::QwenTokenizer& tokenizer,
                   std::span<const std::uint32_t> depths,
                   std::uint32_t selected_concurrency) {
  using Clock = std::chrono::steady_clock;
  const auto elapsed = [](auto start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
  };
  const auto pattern = tokenizer.Encode(
      "Virtual memory maps pages to storage. Red, green, blue. ");
  std::string error;
  const auto vocab = exec.config().vocab_size;
  std::vector<float> logits(vocab);
  for (const auto depth : depths) {
    const auto prefix_size = depth + 32;
    const auto capacity = prefix_size + 64;
    std::vector<std::int32_t> prefix(prefix_size);
    for (std::size_t i = 0; i < prefix.size(); ++i)
      prefix[i] = pattern[i % pattern.size()];
    auto base = exec.CreateSession(gufo::core::SessionMode::kSpeculative,
                                   capacity, &error);
    Require(base != nullptr, error);
    for (std::size_t offset = 0; offset < prefix.size();) {
      const auto n =
          std::min<std::size_t>(exec.max_batch(), prefix.size() - offset);
      Require(
          exec.Forward(*base, std::span(prefix).subspan(offset, n), 0, nullptr,
                       q::rocm::Executor::ForwardMode::kPrefill, &error),
          error);
      const auto next = prefix[(offset + n) % prefix.size()];
      Require(
          exec.MtpForward(*base, std::span(&next, 1),
                          std::min<std::size_t>(n, exec.max_speculative()) - 1,
                          {}, &error),
          error);
      offset += n;
    }
    std::vector<std::uint8_t> common(exec.SnapshotBytes(*base, 0));
    Require(exec.SaveSnapshot(*base, 0, common, &error), error);
    base.reset();
    for (const unsigned concurrency : {1, 2, 4, 6, 8}) {
      if (selected_concurrency != 0 && selected_concurrency != concurrency)
        continue;
      std::vector<std::unique_ptr<q::rocm::Session>> sessions;
      std::vector<std::vector<std::uint8_t>> snapshots;
      std::array<std::array<std::int32_t, 16>, 8> tails{};
      std::array<std::int32_t, 8> anchors{};
      Require(concurrency == 1, "This audit supports configured C1 only");
      const unsigned count = concurrency;
      for (unsigned i = 0; i < count; ++i) {
        sessions.push_back(exec.CreateSession(
            gufo::core::SessionMode::kSpeculative, capacity, &error));
        Require(sessions.back() != nullptr, error);
        q::rocm::Executor::SnapshotInfo info;
        Require(exec.RestoreSnapshot(*sessions.back(), common, &info, &error),
                error);
        const auto suffix = tokenizer.Encode(
            std::string{"Request "} + std::to_string(i) + ": explain " +
            std::array{"gravity", "a queue", "photosynthesis", "sorting",
                       "the alphabet", "Paris", "astronomy", "a mutex"}[i]);
        for (std::size_t j = 0; j < 16; ++j)
          tails[i][j] = suffix[j % suffix.size()];
        Require(exec.Forward(*sessions.back(), tails[i], 1, logits.data(),
                             q::rocm::Executor::ForwardMode::kPrefill, &error),
                error);
        anchors[i] =
            std::max_element(logits.begin(), logits.end()) - logits.begin();
        Require(exec.MtpForward(*sessions.back(), std::span(&anchors[i], 1), 15,
                                {}, &error),
                error);
        snapshots.emplace_back(exec.SnapshotBytes(*sessions.back(), 16));
        Require(
            exec.SaveSnapshot(*sessions.back(), 16, snapshots.back(), &error),
            error);
      }
      for (const unsigned width : {1, 2, 3, 4, 5, 6, 7, 8}) {
        std::array<std::array<double, 4>, 3> samples{};
        for (unsigned repetition = 0; repetition < 5; ++repetition) {
          q::rocm::Executor::SnapshotInfo info;
          Require(exec.RestoreSnapshot(*sessions[0], snapshots[0], &info,
                                       &error, width - 1), error);
          const auto base_position = sessions[0]->position();
          exec.MtpRewind(*sessions[0], base_position - width);
          // Reset the fixture outside the clock. Live serving retains this
          // target sampler; only its proposal copy belongs to preparation.
          gufo::sampling::SamplingConfig sampling_config{
              .temperature=.35F,.top_k=20,.top_p=.9F,.seed=12345,
              .repeat_penalty=1.F,.repeat_last_n=512};
          std::vector<gufo::sampling::TokenId> history(prefix.begin(),prefix.end());
          history.insert(history.end(), tails[0].begin(), tails[0].end());
          gufo::sampling::SamplerState sampler(sampling_config,history);
          const auto preparation_start = Clock::now();
          std::vector<std::int32_t> replay(tails[0].end() - width + 1,
                                           tails[0].end());
          replay.push_back(anchors[0]);
          std::vector<std::int32_t> chain{anchors[0]};
          chain.reserve(width);
          std::vector<q::MtpProposal> proposals;
          proposals.reserve(width - 1);
          auto draft_rng = width > 1
              ? gufo::sampling::NextRandom(sampler.mutable_rng_state()) : 0;
          std::optional<gufo::sampling::SamplerState> draft_sampler;
          if (width > 1) {
            draft_sampler = sampler;
            draft_sampler->Accept(static_cast<gufo::sampling::TokenId>(anchors[0]));
          }
          q::MtpCandidateLogits candidates;
          const double preparation_ms = elapsed(preparation_start);
          const auto catchup_start = Clock::now();
          Require(exec.MtpForward(*sessions[0],replay,16-width,
                    {.candidates=width>1 ? &candidates : nullptr},&error),error);
          const double catchup_ms = elapsed(catchup_start);
          const auto proposal_start = Clock::now();
          for (unsigned step = 1; step < width; ++step) {
            if (step > 1)
              Require(exec.MtpForward(*sessions[0],std::span(&chain.back(),1),-1,{.candidates=&candidates},&error),error);
            auto proposal=q::SampleMtpProposal(candidates,*draft_sampler,&draft_rng);
            chain.push_back(static_cast<std::int32_t>(proposal.token));
            draft_sampler->Accept(proposal.token);
            proposals.push_back(proposal);
          }
          const double proposal_ms = elapsed(proposal_start);
          const auto target_start = Clock::now();
          Require(exec.Forward(*sessions[0], chain, width,
                                 width == 1 ? logits.data() : nullptr,
                                 width == 1
                                     ? q::rocm::Executor::ForwardMode::kDecode
                                     : q::rocm::Executor::ForwardMode::kVerify,
                                 &error, width>1 ? q::rocm::Executor::LogitTransfer::kCompactVerify64 : q::rocm::Executor::LogitTransfer::kDefault),
                    error);
          sampler.Accept(static_cast<gufo::sampling::TokenId>(anchors[0]));
          unsigned keep = 1;
          std::optional<gufo::sampling::TokenId> correction;
          while (keep < width) {
            const auto compact = exec.CompactVerificationCandidates(width);
            Require(compact.size() == width, "missing compact verification rows");
            auto verified = q::VerifyMtpProposalCompact(
                compact[keep-1], proposals[keep-1], sampler, vocab);
            if (!verified) {
              Require(exec.DownloadVerificationRow(keep-1, &error), error);
              verified = q::VerifyMtpProposal(exec.VerificationLogitRow(keep-1),
                                              proposals[keep-1], sampler);
            }
            if (!verified->accepted) {
              correction = verified->token;
              break;
            }
            sampler.Accept(verified->token);
            ++keep;
          }
          if (width > 1)
            Require(exec.Rollback(*sessions[0], keep, &error,
                                  keep < width ? logits.data() : nullptr), error);
          exec.MtpRewind(*sessions[0], base_position);
          // Charge the resulting next frontier draw to this cycle. A rejection
          // defers its already sampled correction, just as live serving does.
          if (correction) sampler.DeferSample(*correction);
          if (width > 1 && keep == width) {
            const auto compact = exec.CompactVerificationCandidates(width);
            auto next_anchor = q::SampleMtpFrontierCompact(
                compact[width-1], sampler, vocab);
            if (!next_anchor) {
              Require(exec.DownloadRetainedFrontier(*sessions[0], logits.data(),
                                                    &error), error);
              (void)sampler.Sample(logits);
            }
          } else {
            (void)sampler.Sample(logits);
          }
          const double target_ms = elapsed(target_start);
          if (repetition >= 2)
            samples[repetition - 2] = {preparation_ms, catchup_ms, proposal_ms, target_ms};
        }
        const auto total = [](const auto& sample) {
          return sample[0] + sample[1] + sample[2] + sample[3];
        };
        std::sort(
            samples.begin(), samples.end(),
            [&](const auto& a, const auto& b) { return total(a) < total(b); });
        const auto& sample = samples[1];
        std::printf(
            "MTP_COST depth=%u C=%u width=%u preparation_ms=%.4f catchup_ms=%.4f "
            "proposal_ms=%.4f target_ms=%.4f total_ms=%.4f\n",
            depth, concurrency, width, sample[0], sample[1], sample[2], sample[3],
            total(sample));
        std::fflush(stdout);
      }
    }
  }
}
