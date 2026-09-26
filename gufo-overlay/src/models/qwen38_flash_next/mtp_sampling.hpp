#ifndef GUFO_MODELS_QWEN38_FLASH_NEXT_MTP_SAMPLING_HPP_
#define GUFO_MODELS_QWEN38_FLASH_NEXT_MTP_SAMPLING_HPP_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <limits>
#include <span>
#include <stdexcept>

#include "src/core/sampling.hpp"

namespace gufo::models::qwen38_flash_next {

enum class DraftDecision { kAccept, kReject, kStop };

// Sampled-MTP proposal policy. HaloGreedy reproduces the proposal semantics
// used by the older high-acceptance halo-box runtime: the MTP head contributes
// its deterministic top token, while the target sampler alone decides whether
// that token survives. Distribution keeps Gufo's stochastic p/q proposal path
// for A/B testing and workloads where its wider q distribution is preferable.
enum class SampledMtpProposalMode { kHaloGreedy, kDistribution };

inline constexpr std::size_t kMtpCandidates = 256;
inline constexpr std::size_t kMtpVerificationCandidates = 256;

struct MtpCandidateLogits {
  std::array<sampling::TokenId, kMtpCandidates> ids{};
  std::array<float, kMtpCandidates> logits{};
  std::size_t size{0};
};

/// Raw target logits selected on the GPU. IDs always remain original
/// vocabulary IDs; unlike draft candidate sampling, compact verification must
/// preserve them through penalties and target-distribution filtering.
struct MtpVerificationCandidateLogits {
  std::array<sampling::TokenId, kMtpVerificationCandidates> ids{};
  std::array<float, kMtpVerificationCandidates> logits{};
  std::size_t size{0};
};

struct MtpProposal {
  std::array<sampling::TokenId, kMtpCandidates> ids{};
  std::array<float, kMtpCandidates> probabilities{};
  std::size_t size{0};
  sampling::TokenId token{0};
  float probability{0.0F};
  // Proposal confidence is intentionally separate from q probability. A
  // deterministic one-hot proposal has q=1, while its confidence still comes
  // from the MTP head and can be used by the draft-confidence gate.
  float confidence{0.0F};
};

/// The proposal has bounded support; the target distribution stays complete.
/// Integer probability masses sum to 2^24, so the exported F32 q sums to
/// exactly one and is exactly the distribution used to draw the proposal.
inline MtpProposal SampleMtpProposal(const MtpCandidateLogits& candidates,
                                     const sampling::SamplerState& sampler,
                                     std::uint64_t* rng) {
  if (candidates.size == 0 || candidates.size > kMtpCandidates) {
    throw std::invalid_argument("invalid MTP candidate count");
  }
  // Keep original vocabulary IDs through penalties and tie ordering whenever
  // top-k is active. With expanded raw Top-256 support this makes q much less
  // likely to lose a post-penalty top-k token. top_k==0 retains Gufo's bounded
  // compact-proposal behavior; q may be any known proposal distribution because
  // exact target output is still enforced by p/q rejection verification.
  const bool mapped = sampler.config().top_k > 0;
  const auto distribution =
      mapped ? sampler.DistributionMapped(
                   std::span(candidates.logits).first(candidates.size),
                   std::span(candidates.ids).first(candidates.size))
             : sampler.Distribution(
                   std::span(candidates.logits).first(candidates.size),
                   std::span(candidates.ids).first(candidates.size));
  constexpr std::uint32_t units = 1U << 24;
  std::array<std::uint32_t, kMtpCandidates> mass{};
  MtpProposal proposal;
  proposal.size = distribution.entries().size();
  std::uint32_t total = 0;
  for (std::size_t i = 0; i < proposal.size; ++i) {
    const auto& entry = distribution.entries()[i];
    proposal.ids[i] = mapped ? entry.token : candidates.ids[entry.token];
    mass[i] = static_cast<std::uint32_t>(std::floor(entry.value * units));
    total += mass[i];
  }
  mass[0] += units - total;
  const auto draw = static_cast<std::uint32_t>(sampling::Uniform(rng) * units);
  std::uint32_t cumulative = 0;
  bool selected = false;
  for (std::size_t i = 0; i < proposal.size; ++i) {
    proposal.probabilities[i] = static_cast<float>(mass[i]) / units;
    cumulative += mass[i];
    if (!selected && draw < cumulative) {
      proposal.token = proposal.ids[i];
      proposal.probability = proposal.probabilities[i];
      proposal.confidence = proposal.probability;
      selected = true;
    }
  }
  return proposal;
}

/// Deterministic proposal compatible with the older halo-box MTP path. The
/// draft token is the raw MTP argmax (target penalties/temperature do not
/// perturb the draft head). Confidence is the top token's softmax probability
/// inside the raw Top-10, matching the old path's top-k-only confidence idea
/// without changing the one-hot proposal distribution used for verification.
inline MtpProposal HaloGreedyMtpProposal(const MtpCandidateLogits& candidates) {
  if (candidates.size == 0 || candidates.size > kMtpCandidates)
    throw std::invalid_argument("invalid MTP candidate count");

  std::size_t best = 0;
  for (std::size_t i = 0; i < candidates.size; ++i) {
    if (!std::isfinite(candidates.logits[i])) continue;
    if (!std::isfinite(candidates.logits[best]) ||
        candidates.logits[i] > candidates.logits[best] ||
        (candidates.logits[i] == candidates.logits[best] &&
         candidates.ids[i] < candidates.ids[best])) {
      best = i;
    }
  }
  if (!std::isfinite(candidates.logits[best]))
    throw std::invalid_argument("MTP candidate logits contain no finite value");

  std::array<double, 10> top{};
  top.fill(-std::numeric_limits<double>::infinity());
  for (std::size_t i = 0; i < candidates.size; ++i) {
    const double value = candidates.logits[i];
    if (!std::isfinite(value) || value <= top.back()) continue;
    std::size_t j = top.size() - 1;
    while (j > 0 && value > top[j - 1]) {
      top[j] = top[j - 1];
      --j;
    }
    top[j] = value;
  }
  const double max_value = top[0];
  double denom = 0.0;
  for (double value : top) {
    if (std::isfinite(value)) denom += std::exp(value - max_value);
  }

  MtpProposal proposal;
  proposal.ids[0] = candidates.ids[best];
  proposal.probabilities[0] = 1.0F;
  proposal.size = 1;
  proposal.token = candidates.ids[best];
  proposal.probability = 1.0F;
  proposal.confidence =
      denom > 0.0 ? static_cast<float>(1.0 / denom) : 1.0F;
  return proposal;
}

struct MtpVerification {
  sampling::TokenId token;
  bool accepted;
};

inline void ValidateMtpProposal(const MtpProposal& proposal,
                                std::size_t vocab_size) {
  if (proposal.size == 0 || proposal.size > kMtpCandidates ||
      proposal.token >= vocab_size || !std::isfinite(proposal.probability) ||
      proposal.probability <= 0 || proposal.probability > 1)
    throw std::invalid_argument("invalid MTP proposal");
  double total = 0;
  double selected = 0;
  for (std::size_t i = 0; i < proposal.size; ++i) {
    if (proposal.ids[i] >= vocab_size ||
        !std::isfinite(proposal.probabilities[i]) ||
        proposal.probabilities[i] < 0)
      throw std::invalid_argument("invalid MTP proposal mass");
    total += proposal.probabilities[i];
    if (proposal.ids[i] == proposal.token)
      selected += proposal.probabilities[i];
  }
  if (total != 1 || selected != proposal.probability)
    throw std::invalid_argument("MTP proposal masses do not match the draw");
}

/// Exact compact verification does not need a fixed Top-256 selector. Pick the
/// smallest graph-stable power-of-two support that leaves at least one raw
/// boundary token outside the authoritative top-k. The boundary is required by
/// the certificate below: repeat penalty >= 1 can only demote selected tokens,
/// so an adjusted kept token strictly above that raw boundary proves no unseen
/// vocabulary token can enter the retained support.
inline constexpr std::size_t CompactMtpVerificationWidthForKeep(
    std::size_t keep) noexcept {
  if (keep < 64) return 64;
  if (keep < 128) return 128;
  if (keep < kMtpVerificationCandidates) return kMtpVerificationCandidates;
  return 0;
}

inline std::size_t CompactMtpVerificationCandidateCount(
    const sampling::SamplerState& sampler) noexcept {
  const auto& config = sampler.config();
  if (!config.uses_random_sampling() || config.top_k <= 0 ||
      config.repeat_penalty < 1.0F || config.frequency_penalty != 0.0F ||
      config.presence_penalty != 0.0F) {
    return 0;
  }
  const std::size_t keep = std::max<std::size_t>(
      static_cast<std::size_t>(config.top_k),
      std::max<std::size_t>(config.min_keep, 1));
  return CompactMtpVerificationWidthForKeep(keep);
}

inline bool CanUseCompactMtpVerification(
    const sampling::SamplerState& sampler) noexcept {
  return CompactMtpVerificationCandidateCount(sampler) != 0;
}

namespace detail {

struct AdjustedCandidate {
  sampling::TokenId token{0};
  double score{0.0};
};

inline double CompactAdjustedLogit(const sampling::SamplerState& sampler,
                                   sampling::TokenId token,
                                   float raw_logit) {
  double adjusted = static_cast<double>(raw_logit);
  const auto& config = sampler.config();
  const auto penalties = sampler.penalties();
  const auto found = std::ranges::lower_bound(penalties, token, {},
                                               &sampling::TokenPenalty::token);
  if (found != penalties.end() && found->token == token && found->repeated &&
      config.repeat_penalty != 1.0F) {
    adjusted = adjusted <= 0.0 ? adjusted * config.repeat_penalty
                               : adjusted / config.repeat_penalty;
  }
  return adjusted;
}

/// Proves that the authoritative post-penalty top-k is completely contained
/// in the selected raw support (Top-64/128/256). Strict > is intentional: a
/// tie at the raw boundary may hide a same-score vocabulary token outside the
/// compact set. A selector that covered the whole vocabulary needs no boundary.
inline bool CertifyCompactSupport(
    const MtpVerificationCandidateLogits& candidates,
    const sampling::SamplerState& sampler, std::size_t vocab_size) {
  const auto required = CompactMtpVerificationCandidateCount(sampler);
  if (required == 0 || candidates.size == 0 ||
      candidates.size > kMtpVerificationCandidates ||
      candidates.size > vocab_size ||
      candidates.size < std::min<std::size_t>(vocab_size, required)) {
    return false;
  }
  const auto& config = sampler.config();
  const std::size_t keep = std::min<std::size_t>(
      candidates.size,
      std::max<std::size_t>(static_cast<std::size_t>(config.top_k),
                            std::max<std::size_t>(config.min_keep, 1)));
  if (keep == 0) return false;

  std::array<AdjustedCandidate, kMtpVerificationCandidates> adjusted{};
  for (std::size_t i = 0; i < candidates.size; ++i) {
    if (!std::isfinite(candidates.logits[i])) return false;
    const double score =
        CompactAdjustedLogit(sampler, candidates.ids[i], candidates.logits[i]);
    if (!std::isfinite(score)) return false;
    adjusted[i] = {.token = candidates.ids[i], .score = score};
  }
  std::ranges::sort(
      std::span(adjusted).first(candidates.size),
      [](const AdjustedCandidate& left, const AdjustedCandidate& right) {
        if (left.score == right.score) return left.token < right.token;
        return left.score > right.score;
      });

  // If the selector covered the whole vocabulary there is no unseen token.
  if (candidates.size == vocab_size) return true;
  const double boundary =
      static_cast<double>(candidates.logits[candidates.size - 1]);
  return adjusted[keep - 1].score > boundary;
}

}  // namespace detail

/// Samples the already-computed target frontier from compact raw Top-64/128/256
/// logits when the same strict support certificate proves that the request's
/// authoritative filtered distribution is fully contained in the compact set.
/// No RNG is consumed on certificate failure, so callers can fall back to the
/// ordinary full-vocabulary sampler without changing seeded behavior.
inline std::optional<sampling::TokenId> SampleMtpFrontierCompact(
    const MtpVerificationCandidateLogits& candidates,
    sampling::SamplerState& sampler, std::size_t vocab_size) {
  if (!detail::CertifyCompactSupport(candidates, sampler, vocab_size))
    return std::nullopt;
  for (std::size_t i = 0; i < candidates.size; ++i) {
    if (candidates.ids[i] >= vocab_size)
      throw std::invalid_argument(
          "compact MTP frontier token exceeds vocabulary");
  }
  auto target = sampler.DistributionMapped(
      std::span(candidates.logits).first(candidates.size),
      std::span(candidates.ids).first(candidates.size));
  return target.Sample(sampler.mutable_rng_state());
}

/// Exact compact sampled verification. The mapped distribution helper in the
/// authoritative sampler applies the same penalties/top-k/top-p/min-p/
/// temperature logic while retaining original vocabulary IDs. No RNG is
/// consumed unless the support certificate succeeds.
inline std::optional<MtpVerification> VerifyDeterministicMtpProposalCompact(
    const MtpVerificationCandidateLogits& candidates,
    sampling::TokenId draft, sampling::SamplerState& sampler,
    std::size_t vocab_size) {
  if (draft >= vocab_size)
    throw std::invalid_argument("deterministic MTP draft exceeds vocabulary");
  if (!detail::CertifyCompactSupport(candidates, sampler, vocab_size))
    return std::nullopt;
  for (std::size_t i = 0; i < candidates.size; ++i) {
    if (candidates.ids[i] >= vocab_size)
      throw std::invalid_argument("compact MTP target token exceeds vocabulary");
  }
  auto target = sampler.DistributionMapped(
      std::span(candidates.logits).first(candidates.size),
      std::span(candidates.ids).first(candidates.size));
  const auto token = target.Sample(sampler.mutable_rng_state());
  return MtpVerification{token, token == draft};
}

inline MtpVerification VerifyDeterministicMtpProposal(
    std::span<const float> logits, sampling::TokenId draft,
    sampling::SamplerState& sampler) {
  if (draft >= logits.size())
    throw std::invalid_argument("deterministic MTP draft exceeds vocabulary");
  const auto token = sampler.Sample(logits);
  return MtpVerification{token, token == draft};
}

inline std::optional<MtpVerification> VerifyMtpProposalCompact(
    const MtpVerificationCandidateLogits& candidates,
    const MtpProposal& proposal, sampling::SamplerState& sampler,
    std::size_t vocab_size) {
  ValidateMtpProposal(proposal, vocab_size);
  if (!detail::CertifyCompactSupport(candidates, sampler, vocab_size))
    return std::nullopt;
  for (std::size_t i = 0; i < candidates.size; ++i) {
    if (candidates.ids[i] >= vocab_size)
      throw std::invalid_argument("compact MTP target token exceeds vocabulary");
  }
  const auto target = sampler.DistributionMapped(
      std::span(candidates.logits).first(candidates.size),
      std::span(candidates.ids).first(candidates.size));
  if (sampler.Uniform() * proposal.probability <
      target.probability(proposal.token)) {
    return MtpVerification{proposal.token, true};
  }
  return MtpVerification{
      target.SampleResidual(std::span(proposal.ids).first(proposal.size),
                            std::span(proposal.probabilities).first(proposal.size),
                            sampler.mutable_rng_state()),
      false};
}

/// The same FP64 target distribution drives AR draws, support, acceptance
/// and residual correction. Only proposal q uses the compact F32 masses.
inline MtpVerification VerifyMtpProposal(std::span<const float> logits,
                                         const MtpProposal& proposal,
                                         sampling::SamplerState& sampler) {
  ValidateMtpProposal(proposal, logits.size());
  const auto target = sampler.Distribution(logits);
  if (sampler.Uniform() * proposal.probability <
      target.probability(proposal.token))
    return {proposal.token, true};
  return {target.SampleResidual(
              std::span(proposal.ids).first(proposal.size),
              std::span(proposal.probabilities).first(proposal.size),
              sampler.mutable_rng_state()),
          false};
}

/// The draft is deterministic, so an exact target sample decides acceptance.
/// A rejected draw is deferred to the next cycle: restore its RNG so that
/// stopping at this prefix consumes exactly the draws made by ordinary decode.
template<typename IsStop>
DraftDecision VerifyDraft(std::span<const float> logits, std::int32_t draft,
                          sampling::SamplerState& sampler, IsStop is_stop) {
  const auto rng = sampler.rng_state();
  const auto token = sampler.Sample(logits);
  if (is_stop(static_cast<std::int32_t>(token))) {
    return DraftDecision::kStop;
  }
  if (token != static_cast<sampling::TokenId>(draft)) {
    sampler.SetRngState(rng);
    return DraftDecision::kReject;
  }
  sampler.Accept(token);
  return DraftDecision::kAccept;
}

}  // namespace gufo::models::qwen38_flash_next

#endif
