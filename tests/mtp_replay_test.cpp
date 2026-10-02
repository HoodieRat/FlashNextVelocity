#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "src/models/qwen/chat_template.hpp"
#include "src/models/qwen38_flash_next/engine.hpp"

namespace q = gufo::models::qwen38_flash_next;
namespace qt = gufo::tokenization;
namespace sampling = gufo::sampling;
static void Require(bool ok, const std::string& error) { if (!ok) throw std::runtime_error(error); }

struct Replay {
  std::vector<std::int32_t> tokens;
  std::vector<std::array<std::uint32_t, 4>> rounds;
  std::uint64_t rng;
  bool operator==(const Replay&) const = default;
};

static Replay Generate(q::Session& session, std::span<const std::int32_t> prompt, unsigned seed) {
  std::string error;
  const sampling::SamplingConfig config{.temperature=.35F, .top_k=20, .top_p=.9F, .seed=seed};
  const std::vector<sampling::TokenId> history(prompt.begin(), prompt.end());
  sampling::SamplerState sampler(config, history);
  session.ResetDraftPolicy(); session.SetDraftConfidence(.75F);
  Require(session.BeginLookupRequest(q::ContextLookupMode::kFixed6, &error),error);
  Replay result;
  while (result.tokens.size() < 128) {
    q::Session::DecodeResult round;
    Require(session.DecodeStep(128-result.tokens.size(), sampler, &round, &error, false), error);
    Require(!round.tokens.empty(), "Decode made no progress");
    result.tokens.insert(result.tokens.end(), round.tokens.begin(), round.tokens.end());
    result.rounds.push_back({static_cast<std::uint32_t>(round.tokens.size()),round.drafted,round.accepted,round.depth});
  }
  result.rng = sampler.rng_state();
  return result;
}

int main(int argc, char** argv) {
  if (argc != 4) { std::cerr << "Usage: mtp_replay_test MODEL MTP distribution|halo_greedy\n"; return 2; }
  try {
    std::string error;
    const bool halo = std::string_view(argv[3]) == "halo_greedy";
    q::ModelOptions options;
    options.max_context=32768; options.mtp_model_path=argv[2]; options.max_draft_tokens=7;
    options.prefill_batch=4096; options.draft_vocabulary=q::DraftVocabulary::kLatinText;
    options.sampled_mtp_proposal_mode=halo ? q::SampledMtpProposalMode::kHaloGreedy : q::SampledMtpProposalMode::kDistribution;
    options.context_lookup=true; options.context_lookup_capacity=16;
    options.context_lookup_min_ngram=5; options.context_lookup_max_ngram=7;
    auto model=q::Model::Load(argv[1],options,&error); Require(model != nullptr,error);
    Require(model->HasDistributionQ8C1Costs()==!halo,"Calibrated profile artifact/geometry guard mismatch");
    for (unsigned seed : {1U,2U}) {
      std::string system="Case " + std::to_string(seed) + ". ";
      for (unsigned i=0;i<32;++i) system += "Follow the user instruction carefully and answer accurately. ";
      const std::array messages{
        qt::ChatMessage{qt::ChatRole::kSystem,system},
        qt::ChatMessage{qt::ChatRole::kUser,"Name three rivers in Europe and one fact about each."},
        qt::ChatMessage{qt::ChatRole::kAssistant,"The Danube, the Rhine and the Loire."},
        qt::ChatMessage{qt::ChatRole::kUser,"Now write a short story about one of them."}};
      auto encoded=qt::QwenChatTemplate::RenderAndTokenize(model->tokenizer(),messages,{.enable_thinking=false},&error);
      Require(encoded.has_value(),error);
      const std::vector<std::int32_t> prompt(encoded->begin(),encoded->end());
      Require(prompt.size()>320,"Replay fixture is too short");
      Replay expected; bool first=true;
      for (auto boundary : {0U,320U,static_cast<unsigned>(prompt.size()-7)}) {
        auto session=model->CreateSession(gufo::core::SessionMode::kSpeculative,32768,&error);
        Require(session != nullptr,error);
        if (boundary) Require(session->Sync(std::span(prompt).first(boundary),&error),error);
        Require(session->Sync(prompt,&error),error);
        auto snapshot=session->SaveSnapshot(&error); Require(snapshot != nullptr,error);
        for (unsigned restore=0;restore<2;++restore) {
          if (restore) Require(session->RestoreSnapshot(*snapshot,&error),error);
          auto got=Generate(*session,prompt,seed);
          if (first) { expected=got; first=false; }
          else Require(got == expected,"MTP replay changed tokens, widths, acceptance, depth, or RNG: seed="+std::to_string(seed)+" boundary="+std::to_string(boundary)+" restore="+std::to_string(restore));
          std::cout << "PASS mode=" << argv[3] << " seed=" << seed << " boundary=" << boundary << " restore=" << restore << " tokens=128 exact=1\n" << std::flush;
        }
        // A shorter prompt forces a rewind/reset before replaying the request.
        Require(session->Sync(std::span(prompt).first(320),&error) && session->Sync(prompt,&error),error);
        Require(Generate(*session,prompt,seed)==expected,"Rewind changed seeded MTP replay");
        std::vector<std::uint8_t> old(snapshot->bytes().begin(),snapshot->bytes().end());
        // Wrapper begins with magic then payload compatibility version.
        auto rejected=old; if (rejected.size()>12) rejected[8] ^= 0x7f;
        Require(!session->RestoreSnapshot(rejected,&error),"Incompatible snapshot was accepted");
      }
    }
    std::cout << "MTP replay checks passed\n";
    return 0;
  } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
