#include "src/models/qwen38_flash_next/engine.hpp"

#include "bench_profile.hpp"


#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>

#include "src/core/gguf_reader.hpp"
#include "src/models/qwen38_flash_next/kernels/rocm/device_model.hpp"
#include "src/models/qwen38_flash_next/kernels/rocm/executor.hpp"
#include "src/models/qwen38_flash_next/kernels/rocm/kernels.hpp"
#include "src/models/qwen38_flash_next/mtp_sampling.hpp"
#include "src/models/qwen38_flash_next/ngram.hpp"
#include "src/models/qwen38_flash_next/weights.hpp"

namespace gufo::models::qwen38_flash_next {
namespace {

// Native prefill max_batch is configurable through ModelOptions::prefill_batch.
// The default remains 2048 because the gfx1151 512/1024/2048/4096 sweep
// favored 2048 while larger chunks only increased scratch in the measured run.

void AssignError(std::string* error_msg, std::string_view message) {
  if (error_msg != nullptr) {
    *error_msg = message;
  }
}

/// DraftVocabulary::kLatinText: special tokens, tokens whose bytes are
/// printable ASCII (or tab / newline / carriage return), and tokens that are
/// complete UTF-8 made of ASCII plus common typographic marks. Pieces of
/// multi-byte characters (most CJK, Cyrillic, accented letters) are left out.
std::vector<std::int32_t> LatinTextVocabulary(
    const tokenization::QwenTokenizer& tokenizer) {
  constexpr char32_t kTypographic[] = {
      0x2014, 0x2013, 0x201C, 0x201D, 0x2018, 0x2019, 0x2026, 0x2022, 0x2192,
      0x2190, 0x2248, 0x00D7, 0x00F7, 0x00B1, 0x00B0, 0x00B7, 0x00E9};
  const auto ascii = [](unsigned char b) {
    return (b >= 0x20 && b < 0x7F) || b == '\t' || b == '\n' || b == '\r';
  };
  std::vector<std::int32_t> ids;
  for (std::size_t id = 0; id < tokenizer.GetVocabSize(); ++id) {
    const auto token = static_cast<tokenization::TokenId>(id);
    if (tokenizer.IsSpecialToken(token)) {
      ids.push_back(static_cast<std::int32_t>(id));
      continue;
    }
    const std::string text = tokenizer.DecodeTokenCopy(token);
    bool keep = !text.empty();
    for (std::size_t i = 0; keep && i < text.size();) {
      const auto b = static_cast<unsigned char>(text[i]);
      if (b < 0x80) {
        keep = ascii(b);
        ++i;
        continue;
      }
      // A complete 2- or 3-byte UTF-8 sequence for an allowed mark.
      const std::size_t length = (b & 0xE0) == 0xC0   ? 2
                                 : (b & 0xF0) == 0xE0 ? 3
                                                      : 0;
      if (length == 0 || i + length > text.size()) {
        keep = false;
        break;
      }
      char32_t cp = length == 2 ? (b & 0x1F) : (b & 0x0F);
      for (std::size_t j = 1; j < length; ++j) {
        const auto c = static_cast<unsigned char>(text[i + j]);
        if ((c & 0xC0) != 0x80) {
          keep = false;
          break;
        }
        cp = (cp << 6) | (c & 0x3F);
      }
      keep = keep && std::find(std::begin(kTypographic), std::end(kTypographic),
                               cp) != std::end(kTypographic);
      i += length;
    }
    if (keep) {
      ids.push_back(static_cast<std::int32_t>(id));
    }
  }
  return ids;
}

constexpr std::array<char, 8> kSessionSnapshotMagic{'Q', 'F', 'N', 'S',
                                                    'E', 'S', 'S', '1'};

/// Host-side session fields ahead of the executor payload: the token
/// history and the logits of the last token.
struct SessionSnapshotHeader {
  std::array<char, 8> magic;
  std::uint32_t version;
  std::uint32_t vocab_size;
  std::uint32_t token_count;
  std::uint32_t hidden_rows;
  std::uint64_t executor_bytes;
  MtpLengthState draft_policy;
  std::uint32_t image_identity_bytes;
  std::uint32_t policy_concurrency;
  std::uint32_t reserved{0};
};
static_assert(std::is_trivially_copyable_v<SessionSnapshotHeader>);

std::uint64_t SessionSnapshotHostBytes(std::uint32_t token_count,
                                       std::uint32_t vocab_size,
                                       std::size_t image_bytes) {
  return sizeof(SessionSnapshotHeader) + image_bytes +
         std::uint64_t{token_count} * sizeof(std::int32_t) +
         std::uint64_t{vocab_size} * sizeof(float);
}

}  // namespace

Model::~Model() = default;

std::shared_ptr<Model> Model::Load(const std::string& model_path,
                                   const ModelOptions& options,
                                   std::string* error_msg) {
  std::shared_ptr<Model> m(new Model());
  if (options.decode_concurrency == 0 || options.decode_concurrency > 8) {
    AssignError(error_msg, "decode concurrency must be between one and eight");
    return nullptr;
  }
  if (options.max_draft_tokens == 0) {
    AssignError(error_msg, "draft token limit must be positive");
    return nullptr;
  }
  if (options.prefill_batch < 128 || options.prefill_batch > 4096) {
    AssignError(error_msg, "prefill batch must be between 128 and 4096 tokens");
    return nullptr;
  }
  if (options.context_lookup &&
      (options.context_lookup_min_ngram < 2 ||
       options.context_lookup_max_ngram < options.context_lookup_min_ngram ||
       options.context_lookup_max_ngram > 32 ||
       options.context_lookup_min_draft == 0 ||
       options.context_lookup_min_draft > kMaxMtpDraftTokens)) {
    AssignError(error_msg, "context lookup settings are invalid");
    return nullptr;
  }
  if (!options.mtp_model_path.empty() &&
      options.max_draft_tokens > kMaxMtpDraftTokens) {
    AssignError(error_msg,
                "Flash-Next MTP supports at most seven draft tokens");
    return nullptr;
  }
  m->options_ = options;
  m->reader_ = core::GgufReader::OpenFile(model_path, error_msg);
  if (!m->reader_) {
    return nullptr;
  }
  auto weights = ModelWeights::Bind(*m->reader_, error_msg);
  if (!weights) {
    return nullptr;
  }
  m->weights_ = std::make_unique<ModelWeights>(std::move(*weights));
  const Config& c = m->weights_->config;
  if (options.max_context == 0 || options.max_context > c.context_length) {
    AssignError(error_msg, "context exceeds the model's " +
                               std::to_string(c.context_length) + " tokens");
    return nullptr;
  }
  try {
    m->vision_ = qwen::vision::Encoder::Open(
        model_path, options.vision_model_path, c.hidden_size);
  } catch (const std::exception& e) {
    AssignError(error_msg, e.what());
    return nullptr;
  }
  m->tokenizer_ =
      tokenization::QwenTokenizer::CreateFromGguf(*m->reader_, error_msg);
  if (!m->tokenizer_) {
    return nullptr;
  }
  if (c.ple_layer >= 0) {
    const auto& t = m->weights_->ple_table;
    m->ngram_ = NgramTable::Open(
        m->reader_->GetMappedRegions()[t.shard].file_descriptor, t.file_offset,
        t.rows, c.ple_head_dim, t.type, error_msg);
    if (!m->ngram_) {
      return nullptr;
    }
  }
  if (!options.mtp_model_path.empty()) {
    m->mtp_reader_ =
        core::GgufReader::OpenFile(options.mtp_model_path, error_msg);
    if (!m->mtp_reader_) {
      return nullptr;
    }
    auto mtp = MtpWeights::Bind(*m->mtp_reader_, c, error_msg);
    if (!mtp) {
      return nullptr;
    }
    m->mtp_weights_ = std::make_unique<MtpWeights>(std::move(*mtp));
  }
  m->device_ = rocm::DeviceModel::Upload(*m->weights_, *m->reader_,
                                         m->mtp_weights_.get(),
                                         m->mtp_reader_.get(), error_msg);
  if (!m->device_) {
    return nullptr;
  }
  rocm::Executor::Options exec;
  exec.max_batch = m->PrefillCapacity();
  exec.max_logit_rows =
      m->mtp_weights_
          ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                exec.max_batch, std::uint64_t{options.max_draft_tokens} + 1))
          : 1;
  exec.max_speculative = exec.max_logit_rows;
  if (m->mtp_weights_ && options.draft_vocabulary == DraftVocabulary::kLatinText) {
    exec.draft_vocab = LatinTextVocabulary(*m->tokenizer_);
  }
  m->executor_ =
      rocm::Executor::Create(*m->device_, m->ngram_.get(), exec, error_msg);
  if (!m->executor_) {
    return nullptr;
  }
  return m;
}

std::unique_ptr<Session> Model::CreateSession(core::SessionMode mode,
                                              std::uint32_t max_context,
                                              std::string* error_msg) {
  if (max_context == 0 || max_context > options_.max_context) {
    AssignError(error_msg, "session context is outside the model limits");
    return nullptr;
  }
  auto native = executor_->CreateSession(mode, max_context, error_msg);
  if (!native) {
    return nullptr;
  }
  return std::unique_ptr<Session>(
      new Session(shared_from_this(), std::move(native)));
}

std::vector<std::int32_t> Model::Tokenize(std::string_view text) const {
  std::vector<std::int32_t> out;
  for (auto id : tokenizer_->Encode(text)) {
    out.push_back(static_cast<std::int32_t>(id));
  }
  return out;
}

std::string Model::Decode(std::span<const std::int32_t> tokens) const {
  std::vector<tokenization::TokenId> ids(tokens.begin(), tokens.end());
  return tokenizer_->Decode(ids);
}

std::string Model::TokenText(std::int32_t token) const {
  return tokenizer_->DecodeTokenCopy(static_cast<tokenization::TokenId>(token));
}

std::int32_t Model::EosToken() const noexcept {
  return static_cast<std::int32_t>(tokenizer_->GetEosTokenId());
}

bool Model::IsStopToken(std::int32_t token) const noexcept {
  return token == EosToken() ||
         token == static_cast<std::int32_t>(tokenizer_->GetPadTokenId());
}

std::uint32_t Model::DraftVocabSize() const noexcept {
  return executor_->draft_vocab_size();
}

std::uint32_t Model::VocabSize() const noexcept {
  return weights_->config.vocab_size;
}

std::uint32_t Model::PrefillCapacity() const noexcept {
  return std::min(options_.prefill_batch, options_.max_context);
}

bool Model::HasMtp() const noexcept {
  return device_->has_mtp();
}

std::string Model::ModelName() const {
  return std::string(reader_->GetMetadataString("general.name")
                         .value_or("Qwen3.8-Flash-Next"));
}

const Config& Model::config() const noexcept {
  return weights_->config;
}

std::size_t Model::ResidentBytes() const noexcept {
  return device_->resident_bytes() + (vision_ ? vision_->ResidentBytes() : 0);
}

std::size_t Model::SessionBytes(core::SessionMode mode,
                                std::uint32_t context) const noexcept {
  const std::size_t vision =
      vision_ ? std::size_t{context} * (config().hidden_size * sizeof(float) +
                                        3 * sizeof(std::int32_t)) +
                    64
              : 0;
  return executor_->SessionBytes(mode, context,
                                 mode == core::SessionMode::kSpeculative
                                     ? executor_->max_speculative() - 1
                                     : 0) +
         vision;
}

std::size_t Model::DeferredScratchBytes() const {
  return executor_->DeferredScratchBytes();
}

std::size_t Session::AllocatedBytes() const noexcept {
  return session_->AllocatedBytes();
}

bool Session::MtpEnabled() const noexcept {
  return session_->mtp_enabled();
}

Session::Session(std::shared_ptr<Model> model,
                 std::unique_ptr<rocm::Session> session)
    : model_(std::move(model)),
      session_(std::move(session)),
      draft_length_(model_->options_.max_draft_tokens,
                    model_->DecodeConcurrency()) {
  logits_.resize(model_->VocabSize());
}

Session::~Session() = default;

std::uint32_t Session::Position() const noexcept {
  return session_->position();
}
std::uint32_t Session::ContextSize() const noexcept {
  return session_->max_context();
}

std::uint64_t Session::MtpFreshResets() const noexcept {
  return model_->executor_->MtpFreshResets();
}

void Session::Reset() {
  valid_ = false;
  session_->Reset();
  tokens_.clear();
  hidden_base_ = 0;
  frontier_candidates_valid_ = false;
  draft_length_.Reset();
  model_->executor_->MtpRewind(*session_, 0);
  valid_ = true;
}

void Session::SetCancellationCheck(std::function<bool()> check) {
  session_->SetCancellationCheck(std::move(check));
}

void Session::ConfigureVision(
    std::shared_ptr<const qwen::vision::Prompt> prompt) {
  const auto identity =
      prompt ? prompt->cache_identity : std::vector<std::uint8_t>{};
  if (!tokens_.empty() && identity != image_identity_)
    Reset();
  const bool was_valid = valid_;
  valid_ = false;
  session_->ConfigureVision(std::move(prompt), model_->vision_,
                            model_->executor_->stream());
  image_identity_ = identity;
  valid_ = was_valid;
}

std::uint32_t Session::KeptHiddenRows() const noexcept {
  return MtpEnabled()
             ? static_cast<std::uint32_t>(tokens_.size() - hidden_base_)
             : 0;
}

std::uint64_t Session::SnapshotBytes() const {
  if (!valid_)
    return 0;
  return SessionSnapshotHostBytes(static_cast<std::uint32_t>(tokens_.size()),
                                  model_->VocabSize(), image_identity_.size()) +
         model_->executor_->SnapshotBytes(*session_, KeptHiddenRows());
}

std::unique_ptr<SessionSnapshot> Session::SaveSnapshot(
    std::string* error_msg) const {
  if (!valid_ || tokens_.empty() || tokens_.size() != session_->position() ||
      tokens_.size() > std::numeric_limits<std::uint32_t>::max()) {
    AssignError(error_msg, "snapshot needs a synced, non-empty context");
    return nullptr;
  }
  const auto token_count = static_cast<std::uint32_t>(tokens_.size());
  const std::uint32_t hidden_rows = KeptHiddenRows();
  const std::uint64_t executor_bytes =
      model_->executor_->SnapshotBytes(*session_, hidden_rows);
  const std::uint64_t host_bytes = SessionSnapshotHostBytes(
      token_count, model_->VocabSize(), image_identity_.size());
  std::unique_ptr<SessionSnapshot> snapshot(
      new SessionSnapshot(host_bytes + executor_bytes));
  std::uint8_t* out = snapshot->data_.get();
  const SessionSnapshotHeader header{
      .magic = kSessionSnapshotMagic,
      .version = kSnapshotPayloadVersion,
      .vocab_size = model_->VocabSize(),
      .token_count = token_count,
      .hidden_rows = hidden_rows,
      .executor_bytes = executor_bytes,
      .draft_policy = draft_length_.State(),
      .image_identity_bytes =
          static_cast<std::uint32_t>(image_identity_.size()),
      .policy_concurrency = model_->DecodeConcurrency(),
  };
  std::memcpy(out, &header, sizeof(header));
  out += sizeof(header);
  if (!image_identity_.empty())
    std::memcpy(out, image_identity_.data(), image_identity_.size());
  out += image_identity_.size();
  std::memcpy(out, tokens_.data(), tokens_.size() * sizeof(std::int32_t));
  out += tokens_.size() * sizeof(std::int32_t);
  // An all-accepted compact-verification cycle intentionally leaves the full
  // target frontier on the GPU. Snapshotting is cold-path work, so materialize
  // that authoritative row here rather than forcing every decode round to do it.
  std::vector<float> snapshot_frontier;
  const float* snapshot_logits = logits_.data();
  if (frontier_candidates_valid_ && MtpEnabled()) {
    snapshot_frontier.resize(model_->VocabSize());
    if (!model_->executor_->DownloadRetainedFrontier(
            *session_, snapshot_frontier.data(), error_msg)) {
      return nullptr;
    }
    snapshot_logits = snapshot_frontier.data();
  }
  std::memcpy(out, snapshot_logits, logits_.size() * sizeof(float));
  out += logits_.size() * sizeof(float);
  if (!model_->executor_->SaveSnapshot(
          *session_, hidden_rows,
          std::span<std::uint8_t>(out,
                                  static_cast<std::size_t>(executor_bytes)),
          error_msg)) {
    return nullptr;
  }
  return snapshot;
}

bool Session::RestoreSnapshot(const SessionSnapshot& snapshot,
                              std::string* error_msg) {
  return RestoreSnapshot(snapshot.bytes(), error_msg);
}

bool Session::RestoreSnapshot(std::span<const std::uint8_t> payload,
                              std::string* error_msg) {
  SessionSnapshotHeader header{};
  if (payload.size() < sizeof(header)) {
    AssignError(error_msg, "session snapshot is truncated");
    return false;
  }
  std::memcpy(&header, payload.data(), sizeof(header));
  if (header.magic != kSessionSnapshotMagic ||
      header.version != kSnapshotPayloadVersion) {
    AssignError(error_msg, "session snapshot format is not supported");
    return false;
  }
  if (header.vocab_size != model_->VocabSize() || header.token_count == 0 ||
      header.policy_concurrency != model_->DecodeConcurrency() ||
      header.token_count > ContextSize() ||
      (header.image_identity_bytes != 0 && header.image_identity_bytes != 32) ||
      payload.size() != SessionSnapshotHostBytes(header.token_count,
                                                 header.vocab_size,
                                                 header.image_identity_bytes) +
                            header.executor_bytes) {
    AssignError(error_msg, "session snapshot does not fit this session");
    return false;
  }
  auto restored_policy = draft_length_;
  if (!restored_policy.Restore(header.draft_policy)) {
    AssignError(error_msg, "session snapshot draft policy is invalid");
    return false;
  }
  const std::uint8_t* in = payload.data() + sizeof(header);
  std::vector<std::uint8_t> image_identity(in,
                                           in + header.image_identity_bytes);
  if (!image_identity.empty() && image_identity != image_identity_) {
    AssignError(error_msg,
                "image snapshot requires its matching prompt attachment");
    return false;
  }
  in += header.image_identity_bytes;
  std::vector<std::int32_t> tokens(header.token_count);
  std::memcpy(tokens.data(), in, tokens.size() * sizeof(std::int32_t));
  in += tokens.size() * sizeof(std::int32_t);
  std::vector<float> logits(header.vocab_size);
  std::memcpy(logits.data(), in, logits.size() * sizeof(float));
  in += logits.size() * sizeof(float);

  if (image_identity.empty())
    ConfigureVision(nullptr);
  valid_ = false;
  rocm::Executor::SnapshotInfo info;
  const auto remaining = ContextSize() - header.token_count;
  const auto next_drafts =
      MtpEnabled() ? restored_policy.Choose(remaining ? remaining - 1 : 0,
                                            header.token_count)
                   : 0;
  if (!model_->executor_->RestoreSnapshot(
          *session_,
          std::span<const std::uint8_t>(
              in, static_cast<std::size_t>(header.executor_bytes)),
          &info, error_msg, next_drafts)) {
    Reset();
    return false;
  }
  if (info.position != header.token_count ||
      info.hidden_rows != header.hidden_rows) {
    Reset();
    AssignError(error_msg, "session snapshot positions are inconsistent");
    return false;
  }
  image_identity_ = std::move(image_identity);
  tokens_ = std::move(tokens);
  logits_ = std::move(logits);
  hidden_base_ = info.position - info.hidden_rows;
  draft_token_ = 0;
  frontier_candidates_valid_ = false;
  draft_length_ = restored_policy;
  stats_ = {};
  valid_ = true;
  return true;
}

SessionSnapshot::SessionSnapshot(std::uint64_t size)
    : data_(new std::uint8_t[size]), size_(size) {}

bool SessionSnapshot::CopyTo(
    std::span<std::uint8_t> destination) const noexcept {
  if (destination.size() != size_) {
    return false;
  }
  std::memcpy(destination.data(), data_.get(), size_);
  return true;
}

bool Session::DraftReplay(std::int32_t next_token,
                          std::vector<std::int32_t>* replay,
                          std::int32_t* hidden_row,
                          std::string* error_msg) const {
  // The draft block trails the trunk: MTP position i consumes token i+1 and
  // the trunk's hidden of position i, so positions up to the current one are
  // replayed once their successor token is known. The session keeps hidden
  // rows of positions [hidden_base_, tokens_.size()).
  rocm::Executor& exec = *model_->executor_;
  const auto size = static_cast<std::uint32_t>(tokens_.size());
  const std::uint32_t mp = exec.MtpPosition(*session_);
  if (mp >= size) {
    return true;
  }
  if (mp < hidden_base_) {
    AssignError(error_msg, "draft block fell behind the kept hidden rows");
    return false;
  }
  replay->assign(tokens_.begin() + mp + 1, tokens_.end());
  replay->push_back(next_token);
  *hidden_row = static_cast<std::int32_t>(mp - hidden_base_);
  return true;
}

bool Session::DraftCatchUp(std::int32_t next_token, bool propose,
                           std::string* error_msg,
                           MtpCandidateLogits* candidates) {
  std::vector<std::int32_t> replay;
  std::int32_t hidden_row = 0;
  if (!DraftReplay(next_token, &replay, &hidden_row, error_msg))
    return false;
  if (replay.empty())
    return true;
  auto& exec = *model_->executor_;
  if (!exec.MtpForward(
          *session_, replay, hidden_row,
          {.token = propose && candidates == nullptr ? &draft_token_ : nullptr,
           .candidates = candidates},
          error_msg)) {
    return false;
  }
  return true;
}

bool Session::DraftCatchUpBatch(std::span<const AdvanceRequest> requests,
                                std::string* error_msg) {
  if (requests.empty())
    return true;
  auto& exec = *requests.front().session->model_->executor_;
  std::vector<std::vector<std::int32_t>> replays(requests.size());
  std::vector<rocm::Executor::MtpBatchItem> items;
  for (std::size_t i = 0; i < requests.size(); ++i) {
    auto& session = *requests[i].session;
    if (!session.MtpEnabled())
      continue;
    std::int32_t hidden_row = 0;
    if (!session.DraftReplay(requests[i].token, &replays[i], &hidden_row,
                             error_msg))
      return false;
    if (replays[i].empty())
      continue;
    items.push_back({session.session_.get(), replays[i], hidden_row});
  }
  return items.empty() || exec.MtpForwardBatch(items, error_msg);
}

bool Session::Feed(std::span<const std::int32_t> tokens, std::string* error_msg,
                   bool prefill) {
  if (!tokens.empty()) frontier_candidates_valid_ = false;
  rocm::Executor& exec = *model_->executor_;
  for (std::size_t off = 0; off < tokens.size(); off += exec.max_batch()) {
    const std::size_t n =
        std::min<std::size_t>(exec.max_batch(), tokens.size() - off);
    const auto chunk = tokens.subspan(off, n);
    if (MtpEnabled() && !tokens_.empty() &&
        !DraftCatchUp(chunk[0], false, error_msg)) {
      return false;
    }
    const auto mode = prefill ? rocm::Executor::ForwardMode::kPrefill
                              : rocm::Executor::ForwardMode::kDecode;
    if (!exec.Forward(*session_, chunk, 1, logits_.data(), mode, error_msg)) {
      return false;
    }
    hidden_base_ = static_cast<std::uint32_t>(
        tokens_.size() + n - std::min<std::size_t>(n, exec.max_speculative()));
    tokens_.insert(tokens_.end(), chunk.begin(), chunk.end());
  }
  return true;
}

bool Session::Sync(std::span<const std::int32_t> prompt,
                   std::string* error_msg) {
  if (prompt.empty()) {
    AssignError(error_msg, "prompt is empty");
    return false;
  }
  if (prompt.size() > ContextSize()) {
    AssignError(error_msg, "prompt exceeds the session context");
    return false;
  }
  if (!valid_)
    Reset();
  // Recurrent state cannot be rewound, so any divergence restarts the
  // session; an extension only feeds the new tail.
  std::size_t common = 0;
  while (common < tokens_.size() && common < prompt.size() &&
         tokens_[common] == prompt[common]) {
    ++common;
  }
  if (common == prompt.size() && common == tokens_.size()) {
    return true;
  }
  if (common != tokens_.size()) {
    Reset();
    common = 0;
  }
  valid_ = false;
  const bool ok = Feed(prompt.subspan(common), error_msg, true);
  valid_ = ok;
  return ok;
}

bool Session::Evaluate(std::int32_t token, std::string* error_msg) {
  if (!valid_) {
    AssignError(error_msg,
                "session needs a successful Sync after a failed operation");
    return false;
  }
  if (tokens_.size() >= ContextSize()) {
    AssignError(error_msg, "session context is full");
    return false;
  }
  valid_ = false;
  const bool ok = Feed(std::span<const std::int32_t>(&token, 1), error_msg);
  valid_ = ok;
  return ok;
}

struct Session::PendingDecode {
  std::vector<std::int32_t> chain;
  std::vector<MtpProposal> proposals;
  std::uint32_t base{0};
  bool speculative{false};
  bool sampled{false};
  bool gpu_greedy{false};
  bool gpu_verification{false};
  bool host_pinned_verification{false};
  bool compact_verification{false};
  std::uint32_t compact_candidates{0};
  std::size_t width{0};
  std::optional<sampling::SamplerState> draft_sampler;
  std::uint64_t draft_rng{0};
  SampledMtpProposalMode proposal_mode{SampledMtpProposalMode::kHaloGreedy};
  bool deterministic_verification{false};
  bool halo_token_only{false};
  MtpCandidateLogits candidates;
  std::int32_t draft{0};
  bool confidence_stopped{false};
  bool lookup{false};
  std::uint32_t lookup_ngram{0};
  std::uint32_t lookup_distance{0};
};

namespace {
MtpProposal DeterministicProposal(std::int32_t token, float confidence = 1.0F) {
  MtpProposal proposal;
  proposal.ids[0] = static_cast<sampling::TokenId>(token);
  proposal.probabilities[0] = 1.0F;
  proposal.size = 1;
  proposal.token = static_cast<sampling::TokenId>(token);
  proposal.probability = 1.0F;
  proposal.confidence = std::clamp(confidence, 0.0F, 1.0F);
  return proposal;
}
}  // namespace

void Session::AppendDraft(PendingDecode& pending) {
  if (pending.sampled) {
    if (pending.proposal_mode == SampledMtpProposalMode::kHaloGreedy) {
      MtpProposal proposal = pending.halo_token_only
                                 ? DeterministicProposal(pending.draft)
                                 : HaloGreedyMtpProposal(pending.candidates);
      pending.draft = static_cast<std::int32_t>(proposal.token);
      pending.proposals.push_back(std::move(proposal));
    } else {
      pending.proposals.push_back(SampleMtpProposal(
          pending.candidates, *pending.draft_sampler, &pending.draft_rng));
      pending.draft = static_cast<std::int32_t>(pending.proposals.back().token);
      pending.draft_sampler->Accept(pending.proposals.back().token);
    }
  }
  pending.chain.push_back(pending.draft);
}

bool Session::DropLowConfidenceDraft(PendingDecode& pending) {
  if (pending.confidence_stopped || !pending.sampled ||
      !(draft_confidence_ > 0.0F) || pending.proposals.empty())
    return false;
  if (!(pending.proposals.back().confidence < draft_confidence_))
    return false;
  pending.proposals.pop_back();
  if (!pending.chain.empty()) pending.chain.pop_back();
  pending.confidence_stopped = true;
  ++stats_.low_confidence_stops;
  return true;
}

bool Session::PrepareDecode(const DecodeRequest& request,
                            PendingDecode* pending, std::string* error_msg,
                            bool defer_head,
                            std::optional<std::uint32_t> batch_drafts) {
  const auto max_tokens = request.max_tokens;
  auto& sampler = *request.sampler;
  auto* result = request.result;
  const bool stop_at_eos = request.stop_at_eos;
  if (result == nullptr || max_tokens == 0 || tokens_.empty()) {
    AssignError(
        error_msg,
        "decode needs an output, a positive budget and a synced prompt");
    return false;
  }
  *result = {};
  const auto is_stop = [&](std::int32_t token) {
    return stop_at_eos && model_->IsStopToken(token);
  };
  rocm::Executor& exec = *model_->executor_;
  const std::size_t room = ContextSize() - tokens_.size();
  const std::size_t cap =
      std::min<std::size_t>({max_tokens, room, exec.max_speculative()});
  const std::size_t width =
      MtpEnabled() && cap > 1
          ? 1 + (batch_drafts ? std::min<std::uint32_t>(*batch_drafts, cap - 1)
                              : draft_length_.Choose(
                                    static_cast<std::uint32_t>(cap - 1),
                                    static_cast<std::uint32_t>(tokens_.size())))
          : cap;
  if (width == 0) {
    result->stop = true;
    return true;
  }
  std::int32_t anchor = 0;
  {
    fnvprof::Scope sample(fnvprof::Stage::Sampling);
    bool sampled_compact_frontier = false;
    // A deferred residual correction must win exactly as SamplerState::Sample
    // would. Otherwise, try the cached raw target shortlist first. Certificate
    // failure consumes no RNG and falls through to the retained full-vocabulary
    // frontier, preserving exact seeded behavior.
    const auto draw_state = sampler.SaveDrawState();
    const bool had_compact_frontier = frontier_candidates_valid_;
    if (!draw_state.pending && had_compact_frontier) {
      const auto compact = SampleMtpFrontierCompact(
          frontier_candidates_, sampler, model_->VocabSize());
      if (compact) {
        anchor = static_cast<std::int32_t>(*compact);
        sampled_compact_frontier = true;
        ++stats_.compact_frontier_samples;
      } else {
        ++stats_.compact_frontier_fallbacks;
      }
    }
    frontier_candidates_valid_ = false;
    if (!sampled_compact_frontier) {
      // The compact verifier no longer downloads the full final frontier on
      // every round. Pull the session-owned GPU row only when the compact
      // support certificate actually fails. Pending residual corrections use
      // sampler state and therefore bypass this cold fallback.
      if (!draw_state.pending && had_compact_frontier) {
        if (!exec.DownloadRetainedFrontier(*session_, logits_.data(), error_msg)) {
          return false;
        }
        ++stats_.retained_frontier_downloads;
      }
      anchor = static_cast<std::int32_t>(sampler.Sample(logits_));
    }
  }
  if (is_stop(anchor)) {
    result->stop = true;
    return true;
  }
  if (!MtpEnabled()) {
    pending->chain = {anchor};
    pending->base = static_cast<std::uint32_t>(tokens_.size());
    return true;
  }

  const std::uint32_t base = static_cast<std::uint32_t>(tokens_.size());
  const auto validate_mtp_position = [&]() {
    const auto actual = exec.MtpPosition(*session_);
    if (actual == base) return true;
    AssignError(error_msg,
                "MTP recurrent-state lifecycle mismatch after catch-up: expected " +
                    std::to_string(base) + ", got " + std::to_string(actual));
    return false;
  };
  const bool sampled = sampler.config().uses_random_sampling();
  const bool gpu_greedy = sampler.config().can_use_unmodified_argmax();
  const bool gpu_verification = gpu_greedy;

  // Prompt/context lookup is a proposal source, not a second decoder. It is
  // single-session only for now so the existing shared MTP batch scheduler is
  // untouched. Its width is capped by configured draft capacity, not the MTP
  // length learner: lookup selection avoids the recursive draft-head proposal
  // work and has different economics. Sampled requests use an exact one-hot q
  // distribution and the same p/q rejection+residual verifier as MTP,
  // preserving target sampling.
  const auto lookup_budget =
      cap > 1 ? std::min<std::size_t>(model_->options_.max_draft_tokens, cap - 1)
              : 0;
  if (!defer_head && model_->options_.context_lookup &&
      lookup_budget >= model_->options_.context_lookup_min_draft) {
    const auto lookup = FindContextLookup(
        tokens_, anchor, static_cast<std::uint32_t>(lookup_budget),
        model_->options_.context_lookup_min_ngram,
        model_->options_.context_lookup_max_ngram,
        model_->options_.context_lookup_window,
        model_->options_.context_lookup_min_draft);
    if (lookup) {
      // The lookup lane replaces MTP proposal generation, not MTP recurrent
      // state maintenance. Advance the draft block through the newly known
      // anchor without running its output head so MtpRewind(base) below names
      // a state that was actually computed. This also preserves the hidden-row
      // contract if several lookup cycles occur before the next MTP proposal.
      {
        fnvprof::Scope catchup(fnvprof::Stage::Catchup);
        if (!DraftCatchUp(anchor, false, error_msg) || !validate_mtp_position())
          return false;
      }
      pending->chain.reserve(1 + lookup->continuation.size());
      pending->chain.push_back(anchor);
      pending->chain.insert(pending->chain.end(), lookup->continuation.begin(),
                            lookup->continuation.end());
      if (sampled) {
        pending->proposals.reserve(lookup->continuation.size());
        for (const auto token : lookup->continuation)
          pending->proposals.push_back(DeterministicProposal(token));
      }
      pending->base = base;
      pending->width = pending->chain.size();
      pending->speculative = true;
      pending->sampled = sampled;
      pending->proposal_mode = model_->options_.sampled_mtp_proposal_mode;
      pending->deterministic_verification = sampled;
      pending->gpu_greedy = gpu_greedy;
      pending->gpu_verification = gpu_verification;
      pending->host_pinned_verification = !gpu_verification;
      pending->compact_candidates =
          pending->host_pinned_verification && sampled
              ? static_cast<std::uint32_t>(
                    CompactMtpVerificationCandidateCount(sampler))
              : 0;
      pending->compact_verification = pending->compact_candidates != 0;
      if (pending->host_pinned_verification && sampled &&
          !pending->compact_verification) {
        fnvprof::NoteUnsupportedSamplerFallback();
      }
      pending->lookup = true;
      pending->lookup_ngram = lookup->ngram;
      pending->lookup_distance = lookup->distance;
      return true;
    }
  }

  // The MTP policy may deliberately schedule an autoregressive cooldown.
  // Context lookup is checked before this gate because it is an independent
  // proposal source with different economics. If lookup did not match, retain
  // the original headless-MTP catch-up behavior for the AR cycle.
  if (width < 2) {
    if (!defer_head) {
      fnvprof::Scope catchup(fnvprof::Stage::Catchup);
      if (!DraftCatchUp(anchor, false, error_msg) || !validate_mtp_position())
        return false;
    }
    pending->chain = {anchor};
    pending->base = base;
    return true;
  }

  const auto proposal_mode = model_->options_.sampled_mtp_proposal_mode;
  // Halo-compatible deterministic proposals do not need a private proposal RNG
  // or a cloned target sampler. With draft confidence disabled they also need
  // only the 4-byte MTP argmax, avoiding the Top-256 proposal transfer.
  const bool halo_token_only = sampled &&
      proposal_mode == SampledMtpProposalMode::kHaloGreedy &&
      !(draft_confidence_ > 0.0F);
  const bool need_candidates = sampled && !halo_token_only;
  const bool async_halo_chain =
      !defer_head && halo_token_only && width >= 2;
  if (!async_halo_chain) {
    fnvprof::Scope catchup(fnvprof::Stage::Catchup);
    if (!defer_head &&
        (!DraftCatchUp(anchor, true, error_msg,
                       need_candidates ? &pending->candidates : nullptr) ||
         !validate_mtp_position())) {
      return false;
    }
  }
  if (sampled && proposal_mode == SampledMtpProposalMode::kDistribution) {
    // Gufo distribution mode retains its independent proposal stream. Halo
    // mode deliberately consumes no target RNG here; only target verification
    // draws advance the request sampler, matching the old high-acceptance path.
    pending->draft_rng = sampling::NextRandom(sampler.mutable_rng_state());
    pending->draft_sampler = sampler;
    pending->draft_sampler->Accept(static_cast<sampling::TokenId>(anchor));
  }
  pending->chain = {anchor};
  pending->draft = draft_token_;
  pending->width = width;
  pending->base = base;
  pending->speculative = true;
  pending->sampled = sampled;
  pending->proposal_mode = proposal_mode;
  pending->deterministic_verification =
      sampled && proposal_mode == SampledMtpProposalMode::kHaloGreedy;
  pending->halo_token_only = halo_token_only;
  pending->gpu_greedy = gpu_greedy;
  pending->gpu_verification = gpu_verification;
  pending->host_pinned_verification = !gpu_verification && !defer_head;
  pending->compact_candidates =
      pending->host_pinned_verification && sampled
          ? static_cast<std::uint32_t>(
                CompactMtpVerificationCandidateCount(sampler))
          : 0;
  pending->compact_verification = pending->compact_candidates != 0;
  if (pending->host_pinned_verification && sampled &&
      !pending->compact_verification) {
    fnvprof::NoteUnsupportedSamplerFallback();
  }
  // Concurrent/batched verification still uses its existing per-session
  // destination. Single-session verification consumes synchronized pinned
  // executor rows directly, removing the redundant full host copy.
  if (!gpu_verification && !pending->host_pinned_verification &&
      verify_logits_.empty()) {
    verify_logits_.resize(exec.max_speculative() * model_->VocabSize());
  }
  if (!defer_head) {
    fnvprof::Scope proposal(fnvprof::Stage::Proposal);
    if (async_halo_chain) {
      std::vector<std::int32_t> replay;
      std::int32_t hidden_row = 0;
      if (!DraftReplay(anchor, &replay, &hidden_row, error_msg) ||
          replay.empty()) {
        AssignError(error_msg, "async halo MTP catch-up replay is unavailable");
        return false;
      }
      std::vector<std::int32_t> drafts(width - 1);
      if (!exec.MtpHaloGreedyChain(*session_, replay, hidden_row, drafts,
                                   error_msg)) {
        return false;
      }
      ++stats_.async_halo_chains;
      stats_.async_halo_draft_tokens += drafts.size();
      const auto expected =
          base + static_cast<std::uint32_t>(drafts.size() - 1);
      if (exec.MtpPosition(*session_) != expected) {
        AssignError(error_msg,
                    "async halo MTP lifecycle mismatch: expected " +
                        std::to_string(expected) + ", got " +
                        std::to_string(exec.MtpPosition(*session_)));
        return false;
      }
      pending->chain.reserve(width);
      for (const auto token : drafts) {
        pending->draft = token;
        pending->proposals.push_back(DeterministicProposal(token));
        pending->chain.push_back(token);
        ++stats_.mtp_confidence_samples;
        stats_.mtp_confidence_sum += 1.0;
      }
    } else {
      while (pending->chain.size() < width && !pending->confidence_stopped) {
        AppendDraft(*pending);
        if (sampled && !pending->proposals.empty()) {
          ++stats_.mtp_confidence_samples;
          stats_.mtp_confidence_sum += pending->proposals.back().confidence;
        }
        if (DropLowConfidenceDraft(*pending)) break;
        if (pending->chain.size() < width &&
            !exec.MtpForward(
                *session_, std::span<const std::int32_t>(&pending->draft, 1), -1,
                {.token = (!sampled || pending->halo_token_only) ? &pending->draft : nullptr,
                 .candidates = (sampled && !pending->halo_token_only) ? &pending->candidates : nullptr},
                error_msg)) {
          return false;
        }
      }
    }
  }
  // Draft-confidence gating can reject the very first sampled proposal. In
  // that case this remains a speculative target pass (the MTP state/catch-up
  // path is already prepared), but there are zero draft rows to compact. Fall
  // back to the existing pinned full frontier row instead of asking the compact
  // transfer path to process n_logits == 1.
  if (pending->compact_verification && pending->chain.size() < 2) {
    pending->compact_verification = false;
    pending->compact_candidates = 0;
  }
  return true;
}

bool Session::FinishDecode(const DecodeRequest& request,
                           const PendingDecode& pending,
                           std::string* error_msg) {
  auto& sampler = *request.sampler;
  auto* result = request.result;
  auto& exec = *model_->executor_;
  const auto& chain = pending.chain;
  const auto& proposals = pending.proposals;
  const auto base = pending.base;
  const bool sampled = pending.sampled;
  const bool gpu_greedy = pending.gpu_greedy;
  const bool gpu_verification = pending.gpu_verification;
  const bool host_pinned_verification = pending.host_pinned_verification;
  const bool compact_verification = pending.compact_verification;
  const auto anchor = chain.front();
  const auto k = static_cast<std::uint32_t>(chain.size());
  // Any previous frontier cache corresponds to the logits consumed to choose
  // this cycle's anchor. A new one is published only after a successful
  // all-accepted compact verification below.
  frontier_candidates_valid_ = false;
  const auto vocab = model_->VocabSize();
  const auto is_stop = [&](std::int32_t token) {
    return request.stop_at_eos && model_->IsStopToken(token);
  };
  if (!pending.speculative) {
    draft_length_.ObserveArToken();
    hidden_base_ = base;
    tokens_.push_back(anchor);
    sampler.Accept(static_cast<sampling::TokenId>(anchor));
    result->tokens.push_back(anchor);
    result->drafted = 0;
    result->accepted = 0;
    result->depth = 0;
    fnvprof::EndCycle(0, 0, 0, 1, fnvprof::ProposalSource::None);
    return true;
  }
  sampler.Accept(static_cast<sampling::TokenId>(anchor));
  std::array<rocm::ArgmaxCandidate, kMaxMtpDraftTokens> greedy{};
  std::uint32_t keep = 1;
  std::optional<std::int32_t> correction;
  {
    fnvprof::Scope decision_time(fnvprof::Stage::Decision);
    if (gpu_greedy &&
        !exec.GreedyMtpPredictions(std::span(greedy).first(k - 1), error_msg)) {
      return false;
    }
  while (keep < k) {
    if (gpu_greedy) {
      const auto& prediction = greedy[keep - 1];
      if (!std::isfinite(prediction.value)) {
        AssignError(error_msg, "logit distribution contains no finite values");
        return false;
      }
      if (is_stop(prediction.index)) {
        result->stop = true;
        break;
      }
      if (prediction.index != chain[keep]) {
        break;
      }
      sampler.Accept(static_cast<sampling::TokenId>(prediction.index));
      ++keep;
      continue;
    }
    if (sampled) {
      MtpVerification verified{};
      const auto row = keep - 1;
      bool used_compact = false;
      if (compact_verification) {
        const auto compact_rows =
            exec.CompactVerificationCandidates(k > 0 ? k - 1 : 0);
        if (row >= compact_rows.size()) {
          AssignError(error_msg,
                      "compact verification row is outside candidate buffer");
          return false;
        }
        std::optional<MtpVerification> compact;
        {
          fnvprof::VerifyCpuScope cpu_verify;
          compact = pending.deterministic_verification
                        ? VerifyDeterministicMtpProposalCompact(
                              compact_rows[row],
                              static_cast<sampling::TokenId>(chain[keep]),
                              sampler, vocab)
                        : VerifyMtpProposalCompact(compact_rows[row],
                                                   proposals[row], sampler,
                                                   vocab);
        }
        if (compact) {
          fnvprof::NoteCertificateSuccess();
          verified = *compact;
          used_compact = true;
        } else {
          fnvprof::NoteCertificateFallback();
          // The target row already exists on the GPU. Download it only after
          // the CPU certificate has failed, outside CPU-accept profiling.
          if (!exec.DownloadVerificationRow(row, error_msg)) {
            return false;
          }
        }
      }
      if (!used_compact) {
        std::span<const float> target_logits;
        if (host_pinned_verification) {
          target_logits = exec.VerificationLogitRow(row);
        } else {
          target_logits = std::span<const float>(verify_logits_)
                              .subspan(static_cast<std::size_t>(row) * vocab,
                                       vocab);
        }
        if (target_logits.size() != vocab) {
          AssignError(error_msg, "verification target row is unavailable");
          return false;
        }
        fnvprof::VerifyCpuScope cpu_verify;
        verified = pending.deterministic_verification
                       ? VerifyDeterministicMtpProposal(
                             target_logits,
                             static_cast<sampling::TokenId>(chain[keep]), sampler)
                       : VerifyMtpProposal(target_logits, proposals[row], sampler);
      }
      const auto token = static_cast<std::int32_t>(verified.token);
      const bool accepted = verified.accepted;
      if (is_stop(token)) {
        result->stop = true;
        break;
      }
      if (!accepted) {
        correction = token;
        break;
      }
      sampler.Accept(static_cast<sampling::TokenId>(token));
      ++keep;
      continue;
    }
    const auto target_logits =
        host_pinned_verification
            ? exec.VerificationLogitRow(keep - 1)
            : std::span<const float>(verify_logits_)
                  .subspan(static_cast<std::size_t>(keep - 1) * vocab, vocab);
    const auto decision = VerifyDraft(target_logits, chain[keep], sampler, is_stop);
    if (decision != DraftDecision::kAccept) {
      result->stop = decision == DraftDecision::kStop;
      break;
    }
    ++keep;
  }
  }
  {
    fnvprof::Scope rollback_time(fnvprof::Stage::Rollback);
    float* rollback_logits = nullptr;
    if (gpu_verification) {
      rollback_logits = logits_.data();
    } else if (compact_verification && keep < k) {
      // A rejected draft makes this intermediate target row the next
      // autoregressive frontier. Reuse the existing rollback path to download
      // exactly that row while restoring recurrent/KV state.
      fnvprof::NoteRejectionFrontierDownload();
      rollback_logits = logits_.data();
    }
    if (!exec.Rollback(*session_, keep, error_msg, rollback_logits)) {
      return false;
    }
  }
  if (!gpu_verification && !compact_verification) {
    const auto frontier =
        host_pinned_verification
            ? exec.VerificationLogitRow(keep - 1)
            : std::span<const float>(verify_logits_)
                  .subspan(static_cast<std::size_t>(keep - 1) * vocab, vocab);
    if (frontier.size() != vocab) {
      AssignError(error_msg, "verification frontier row is unavailable");
      return false;
    }
    std::copy(frontier.begin(), frontier.end(), logits_.begin());
  }
  // Compact rejection already downloaded the kept rejection frontier through
  // Rollback(). On all-accepted compact cycles the authoritative full frontier
  // deliberately remains GPU-resident; the next anchor normally comes from the
  // certified compact candidates and falls back to DownloadRetainedFrontier().
  if (sampled && compact_verification && keep == k && !result->stop) {
    const auto compact_rows = exec.CompactVerificationCandidates(k);
    if (compact_rows.size() == k) {
      frontier_candidates_ = compact_rows[k - 1];
      frontier_candidates_valid_ = true;
    }
  }
  hidden_base_ = base;
  tokens_.insert(tokens_.end(), chain.begin(), chain.begin() + keep);
  result->tokens.assign(chain.begin(), chain.begin() + keep);
  stats_.cycles += 1;
  stats_.drafted += k - 1;
  stats_.accepted += keep - 1;
  if (pending.lookup) {
    ++stats_.lookup_cycles;
    stats_.lookup_drafted += k - 1;
    stats_.lookup_accepted += keep - 1;
    stats_.lookup_ngram_sum += pending.lookup_ngram;
    stats_.lookup_distance_sum += pending.lookup_distance;
  } else {
    ++stats_.mtp_cycles;
    stats_.mtp_drafted += k - 1;
    stats_.mtp_accepted += keep - 1;
  }
  // A target stop ends the request; it does not classify the remaining
  // proposals as failed predictions. Lookup acceptance is deliberately excluded
  // from the MTP acceptance learner because it measures repeated-context
  // predictability, not draft-head quality. It still advances one AR cooldown
  // tick so a temporarily disabled MTP policy eventually gets its normal retry.
  if (pending.lookup) {
    for (std::uint32_t i = 0; i < keep; ++i) draft_length_.ObserveArToken();
  } else {
    draft_length_.Observe(keep - 1, result->stop ? keep - 1 : k - 1, base);
  }

  // The next call knows the next sampled anchor. Defer draft catch-up until
  // then, retaining this session's target hidden rows across interleaving.
  exec.MtpRewind(*session_, base);
  if (correction) {
    // Evaluate the rejected target draw / residual correction as the next
    // cycle's anchor, avoiding a separate target pass. Preserve the actual
    // draw: resampling p would change the authoritative target stream.
    sampler.DeferSample(static_cast<sampling::TokenId>(*correction));
  }
  result->drafted = k - 1;
  result->accepted = keep - 1;
  result->depth = k - 1;
  result->lookup = pending.lookup;
  result->lookup_ngram = pending.lookup_ngram;
  fnvprof::EndCycle(k - 1, k - 1, keep - 1, keep,
                    pending.lookup ? fnvprof::ProposalSource::Lookup
                                   : fnvprof::ProposalSource::Mtp);
  return true;
}

bool Session::DecodeStep(std::size_t max_tokens,
                         sampling::SamplerState& sampler, DecodeResult* result,
                         std::string* error_msg, bool stop_at_eos) {
  if (!valid_) {
    AssignError(error_msg,
                "session needs a successful Sync after a failed operation");
    return false;
  }
  valid_ = false;
  fnvprof::BeginCycle();
  const DecodeRequest request{this, max_tokens, &sampler, result, stop_at_eos};
  PendingDecode pending;
  if (!PrepareDecode(request, &pending, error_msg)) {
    fnvprof::CancelCycle();
    return false;
  }
  if (pending.chain.empty()) {
    fnvprof::CancelCycle();
    valid_ = true;
    return true;
  }
  float* logits = !pending.speculative
                      ? logits_.data()
                      : (pending.gpu_verification ||
                         pending.host_pinned_verification)
                            ? nullptr
                            : verify_logits_.data();
  auto transfer = rocm::Executor::LogitTransfer::kDefault;
  if (pending.compact_verification) {
    switch (pending.compact_candidates) {
      case 64:
        transfer = rocm::Executor::LogitTransfer::kCompactVerify64;
        break;
      case 128:
        transfer = rocm::Executor::LogitTransfer::kCompactVerify128;
        break;
      case kMtpVerificationCandidates:
        transfer = rocm::Executor::LogitTransfer::kCompactVerify256;
        break;
      default:
        AssignError(error_msg, "unsupported compact verification candidate width");
        fnvprof::CancelCycle();
        return false;
    }
  } else if (pending.host_pinned_verification) {
    transfer = rocm::Executor::LogitTransfer::kPinnedFull;
  }
  {
    fnvprof::Scope forward(pending.speculative ? fnvprof::Stage::VerifyForward
                                               : fnvprof::Stage::PlainForward);
    if (!model_->executor_->Forward(
            *session_, pending.chain, pending.chain.size(), logits,
            pending.speculative ? rocm::Executor::ForwardMode::kVerify
                                : rocm::Executor::ForwardMode::kDecode,
            error_msg, transfer)) {
      fnvprof::CancelCycle();
      return false;
    }
  }
  const bool ok = FinishDecode(request, pending, error_msg);
  if (!ok) fnvprof::CancelCycle();
  valid_ = ok;
  return ok;
}

template<class Request>
bool Session::RunIsolatedBatch(std::span<const Request> requests,
                               std::string* error_msg) {
  if (requests.empty() || requests.size() > 8) {
    AssignError(error_msg, "batch must contain 1..8 sessions");
    return false;
  }
  std::array<BatchOutcome, 8> outcomes{};
  std::array<std::uint64_t, 8> epochs{};
  std::array<sampling::SamplerState::DrawState, 8> draws{};
  std::vector<Request> active;
  active.reserve(requests.size());
  Model* model = nullptr;
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const auto& r = requests[i];
    auto& outcome = outcomes[i];
    if (r.session)
      epochs[i] = r.session->session_->MutationEpoch();
    bool valid = r.session && r.session->valid_;
    if constexpr (std::is_same_v<Request, DecodeRequest>) {
      valid = valid && r.sampler && r.result && r.max_tokens > 0 &&
              !r.session->tokens_.empty();
    } else {
      valid = valid && r.token >= 0 &&
              static_cast<std::uint32_t>(r.token) <
                  r.session->model_->VocabSize() &&
              r.session->Position() < r.session->ContextSize();
    }
    for (std::size_t j = 0; j < requests.size(); ++j) {
      if (i == j)
        continue;
      valid = valid && r.session != requests[j].session;
      if constexpr (std::is_same_v<Request, DecodeRequest>)
        valid = valid && r.sampler != requests[j].sampler &&
                r.result != requests[j].result;
    }
    if (valid && model && r.session->model_.get() != model)
      valid = false;
    if (!valid) {
      outcome.error = "invalid or non-independent batch request";
      continue;
    }
    if (!r.session->session_->CheckCancellation(&outcome.error))
      continue;
    model = r.session->model_.get();
    if constexpr (std::is_same_v<Request, DecodeRequest>) {
      draws[i] = r.sampler->SaveDrawState();
    }
    auto copy = r;
    copy.outcome = &outcome;
    active.push_back(copy);
  }
  std::string shared_error;
  try {
    if (!active.empty()) {
      if constexpr (std::is_same_v<Request, DecodeRequest>)
        (void)DecodeBatchImpl(active, &shared_error);
      else
        (void)EvaluateBatchImpl(active, &shared_error);
    }
  } catch (const std::exception& exception) {
    shared_error = exception.what();
  }
  bool success = true;
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const auto& r = requests[i];
    auto& outcome = outcomes[i];
    if (!outcome.completed && outcome.error.empty()) {
      auto& session = *r.session;
      if (session.session_->Cancelled()) {
        outcome.error = "generation cancelled";
      } else if (session.session_->MutationEpoch() == epochs[i]) {
        // A preparation/batch-allocation failure did not touch this peer.
        // Retry it independently, without changing its cached frontier.
        session.valid_ = true;
        if constexpr (std::is_same_v<Request, DecodeRequest>) {
          r.sampler->RestoreDrawState(draws[i]);
        }
        try {
          if constexpr (std::is_same_v<Request, DecodeRequest>)
            outcome.completed =
                session.DecodeStep(r.max_tokens, *r.sampler, r.result,
                                   &outcome.error, r.stop_at_eos);
          else
            outcome.completed = session.Evaluate(r.token, &outcome.error);
        } catch (const std::exception& exception) {
          outcome.error = exception.what();
        }
      } else {
        outcome.error =
            shared_error.empty() ? "batch execution failed" : shared_error;
      }
    }
    if (!outcome.completed) {
      if (outcome.error.empty())
        outcome.error = "batch request failed";
      if (r.session && r.session->session_->MutationEpoch() != epochs[i])
        r.session->valid_ = false;
      if (success)
        AssignError(error_msg, outcome.error);
      success = false;
    } else {
      r.session->valid_ = true;
    }
    if (r.outcome)
      *r.outcome = std::move(outcome);
  }
  return success;
}

bool Session::DecodeBatch(std::span<const DecodeRequest> requests,
                          std::string* error_msg) {
  return RunIsolatedBatch(requests, error_msg);
}

bool Session::EvaluateBatch(std::span<const AdvanceRequest> requests,
                            std::string* error_msg) {
  return RunIsolatedBatch(requests, error_msg);
}

bool Session::DecodeBatchImpl(std::span<const DecodeRequest> requests,
                              std::string* error_msg) {
  if (requests.size() == 1) {
    const auto& r = requests.front();
    r.outcome->completed = r.session->DecodeStep(
        r.max_tokens, *r.sampler, r.result, &r.outcome->error, r.stop_at_eos);
    return r.outcome->completed;
  }
  auto& exec = *requests.front().session->model_->executor_;
  std::optional<std::uint32_t> batch_drafts;
  std::uint32_t batch_context = 0;
  auto& policy = requests.front().session->model_->batch_policy_;
  if (std::ranges::all_of(
          requests, [](const auto& r) { return r.session->MtpEnabled(); }) &&
      std::ranges::none_of(requests, [](const auto& request) {
        return request.sampler->config().uses_random_sampling();
      })) {
    std::array<MtpBatchController::Row, 8> rows{};
    for (std::size_t i = 0; i < requests.size(); ++i) {
      const auto& r = requests[i];
      const auto cap = std::min<std::size_t>(
          r.max_tokens, r.session->ContextSize() - r.session->Position());
      rows[i] = {&r.session->draft_length_,
                 static_cast<std::uint32_t>(
                     std::min<std::size_t>(cap, exec.max_speculative())) -
                     (cap != 0)};
      batch_context = std::max(batch_context, r.session->Position());
    }
    batch_drafts =
        policy.Choose(std::span(rows).first(requests.size()), batch_context);
  }
  const auto cycle_start = std::chrono::steady_clock::now();
  std::vector<PendingDecode> pending(requests.size());
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const auto& r = requests[i];
    try {
      if (!r.session->PrepareDecode(r, &pending[i], &r.outcome->error, true,
                                    batch_drafts))
        pending[i] = {};
    } catch (const std::exception& exception) {
      r.outcome->error = exception.what();
      pending[i] = {};
    }
  }
  std::vector<AdvanceRequest> catchup;
  for (std::size_t i = 0; i < requests.size(); ++i) {
    if (!pending[i].chain.empty())
      catchup.push_back({requests[i].session, pending[i].chain.front()});
  }
  if (!DraftCatchUpBatch(catchup, error_msg))
    return false;
  // Each round shares predictor projections across ready sessions.
  // Attention state, proposal distributions and RNG streams stay private.
  for (;;) {
    std::vector<rocm::Executor::MtpHeadItem> heads;
    std::vector<rocm::Executor::MtpBatchItem> bodies;
    for (std::size_t i = 0; i < requests.size(); ++i) {
      auto& p = pending[i];
      if (!requests[i].session->session_->Cancelled() && p.speculative &&
          !p.confidence_stopped && p.chain.size() < p.width) {
        heads.push_back({requests[i].session->session_.get(),
                         {.token = p.sampled ? nullptr : &p.draft,
                          .candidates = p.sampled ? &p.candidates : nullptr}});
      }
    }
    if (heads.empty())
      break;
    if (!exec.MtpHeads(heads, error_msg))
      return false;
    for (std::size_t i = 0; i < requests.size(); ++i) {
      auto& p = pending[i];
      if (requests[i].session->session_->Cancelled() || !p.speculative ||
          p.confidence_stopped || p.chain.size() >= p.width)
        continue;
      AppendDraft(p);
      if (requests[i].session->DropLowConfidenceDraft(p)) continue;
      if (p.chain.size() < p.width)
        bodies.push_back({requests[i].session->session_.get(),
                          std::span<const std::int32_t>(&p.draft, 1), -1});
    }
    if (!bodies.empty() && !exec.MtpForwardBatch(bodies, error_msg))
      return false;
  }
  std::vector<rocm::Executor::BatchItem> items;
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const auto& r = requests[i];
    if (!r.session->session_->Cancelled() && !pending[i].chain.empty()) {
      items.push_back({r.session->session_.get(), pending[i].chain,
                       pending[i].speculative});
    }
  }
  if (!items.empty() && !exec.ForwardBatch(items, error_msg))
    return false;
  std::uint32_t offset = 0;
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const auto& p = pending[i];
    const auto& r = requests[i];
    auto& session = *r.session;
    const bool included =
        std::any_of(items.begin(), items.end(), [&](const auto& item) {
          return item.session == session.session_.get();
        });
    const auto row_offset = offset;
    if (included)
      offset += p.chain.size();
    if (!r.outcome->error.empty())
      continue;
    if (session.session_->Cancelled()) {
      r.outcome->error = "generation cancelled";
      continue;
    }
    if (p.chain.empty()) {
      r.outcome->completed = true;
      continue;
    }
    float* logits = !p.speculative       ? session.logits_.data()
                    : p.gpu_verification ? nullptr
                                         : session.verify_logits_.data();
    try {
      r.outcome->completed =
          exec.SelectBatchLogits(row_offset, p.chain.size(), logits,
                                 &r.outcome->error) &&
          session.FinishDecode(r, p, &r.outcome->error);
    } catch (const std::exception& exception) {
      r.outcome->error = exception.what();
    }
  }
  if (batch_drafts && items.size() == requests.size() &&
      std::ranges::all_of(requests,
                          [](const auto& r) { return r.outcome->completed; })) {
    const auto ms = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - cycle_start)
                        .count();
    policy.Observe(requests.size(), batch_context, *batch_drafts, ms);
  }
  return true;
}

bool Session::EvaluateBatchImpl(std::span<const AdvanceRequest> requests,
                                std::string* error_msg) {
  if (requests.size() == 1) {
    const auto& r = requests.front();
    r.outcome->completed = r.session->Evaluate(r.token, &r.outcome->error);
    return r.outcome->completed;
  }
  auto& exec = *requests.front().session->model_->executor_;
  if (!DraftCatchUpBatch(requests, error_msg))
    return false;
  std::vector<rocm::Executor::BatchItem> items;
  for (const auto& r : requests) {
    items.push_back({r.session->session_.get(), {&r.token, 1}, false});
  }
  if (!exec.ForwardBatch(items, error_msg)) {
    return false;
  }
  for (std::size_t i = 0; i < requests.size(); ++i) {
    auto& session = *requests[i].session;
    auto& outcome = *requests[i].outcome;
    if (session.session_->Cancelled()) {
      outcome.error = "generation cancelled";
      continue;
    }
    try {
      if (!exec.SelectBatchLogits(i, 1, session.logits_.data(), &outcome.error))
        continue;
      session.hidden_base_ = static_cast<std::uint32_t>(session.tokens_.size());
      session.tokens_.push_back(requests[i].token);
      outcome.completed = true;
    } catch (const std::exception& exception) {
      outcome.error = exception.what();
    }
  }
  return true;
}

}  // namespace gufo::models::qwen38_flash_next
