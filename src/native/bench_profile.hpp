#pragma once

// Benchmark-only decode profile. Counters stay off during normal API calls.
// HIP event recording lives next to the kernels; this header only stores totals.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fnvprof {

inline constexpr std::uint64_t kProfileGraphBit = std::uint64_t{1} << 36;
inline constexpr int kDepths = 17;
inline constexpr int kStageCount = 9;

enum class Phase : int { Off = 0, Prefill = 1, Decode = 2 };
enum class ProposalSource : int { None = 0, Mtp = 1, Lookup = 2 };

enum class VerifySubstage : int {
  Trunk = 0,
  HcHead = 1,
  VocabProjection = 2,
  CompactSelect = 3,
  D2H = 4,
  CpuAcceptResidual = 5,
};

enum class Stage : int {
  Sampling = 0,
  Catchup = 1,
  Proposal = 2,
  VerifyForward = 3,
  PlainForward = 4,
  Decision = 5,
  Rollback = 6,
};

struct DepthRow {
  std::uint64_t cycles{0};
  double cycle_ms{0};
  std::uint64_t drafted{0};
  std::uint64_t accepted{0};
  std::uint64_t output_tokens{0};
  double proposal_ms{0};
  double verify_ms{0};
};

struct CycleRec {
  std::uint32_t output_tokens{0};
  std::uint32_t drafted{0};
  std::uint32_t accepted{0};
  std::uint32_t depth{0};
  double cycle_ms{0};
  double proposal_ms{0};
  double verify_ms{0};
  double qsa_ms{0};
  double ple_ms{0};
  ProposalSource source{ProposalSource::None};
};

struct ShapeCount {
  int type{0};
  int m{0};
  int n{0};
  int k{0};
  int kind{0};  // 0 = hipBLASLt miss then hipBLAS, 1 = direct hipBLAS
  std::uint64_t count{0};
};

struct StageRow {
  const char* name{""};
  double ms{0};
  double pct{0};
};

struct WindowRow {
  std::uint32_t token_lo{0};
  std::uint32_t token_hi{0};
  double tokens_per_second{0};
  double acceptance{0};
  double avg_draft_depth{0};
  double proposal_ms{0};
  double verify_ms{0};
  double qsa_ms{0};
  double ple_wait_ms{0};
};

struct Report {
  bool present{false};
  double ttft_ms{0};
  std::uint32_t context_capacity{0};
  std::uint32_t context_depth{0};
  std::uint32_t draft_max{0};
  double avg_draft_depth{0};
  double mtp_avg_draft_depth{0};
  double lookup_avg_draft_depth{0};
  double catchup_ms{0};
  double proposal_ms{0};
  double verification_ms{0};
  double rollback_ms{0};
  double mtp_cycle_ms{0};
  std::uint64_t graph_captures{0};
  double verify_trunk_ms{0};
  double verify_hc_head_ms{0};
  double verify_vocab_projection_ms{0};
  double verify_compact_select_ms{0};
  double verify_d2h_ms{0};
  double verify_cpu_accept_residual_ms{0};
  bool verify_gpu_events_reliable{false};
  bool verify_gpu_events_incomplete{false};
  std::uint64_t verify_output_rows{0};
  std::uint64_t verify_full_rows_d2h{0};
  std::uint64_t verify_compact_rows_d2h{0};
  std::uint64_t verify_d2h_bytes{0};
  std::uint64_t verify_compact_candidates{0};
  std::uint64_t verify_certificate_successes{0};
  std::uint64_t verify_certificate_fallbacks{0};
  std::uint64_t verify_unsupported_sampler_fallbacks{0};
  std::uint64_t verify_rejection_frontier_downloads{0};
  std::uint64_t mtp_cycles{0};
  std::uint64_t mtp_drafted{0};
  std::uint64_t mtp_accepted{0};
  std::uint64_t lookup_cycles{0};
  std::uint64_t lookup_drafted{0};
  std::uint64_t lookup_accepted{0};
  std::uint64_t mtp_output_tokens{0};
  std::uint64_t lookup_output_tokens{0};
  double mtp_proposal_ms{0};
  double mtp_verification_ms{0};
  double mtp_total_cycle_ms{0};
  double lookup_proposal_ms{0};
  double lookup_verification_ms{0};
  double lookup_total_cycle_ms{0};
  std::uint64_t proposed_per_position[kDepths - 1]{};
  std::uint64_t accepted_per_position[kDepths - 1]{};
  std::uint64_t mtp_proposed_per_position[kDepths - 1]{};
  std::uint64_t mtp_accepted_per_position[kDepths - 1]{};
  std::uint64_t lookup_proposed_per_position[kDepths - 1]{};
  std::uint64_t lookup_accepted_per_position[kDepths - 1]{};
  DepthRow depths[kDepths]{};
  DepthRow mtp_depths[kDepths]{};
  DepthRow lookup_depths[kDepths]{};
  std::vector<WindowRow> windows;
  double qsa_indexer_ms{0};
  double qsa_selector_ms{0};
  double qsa_sparse_ms{0};
  double qsa_total_ms{0};
  bool qsa_events_reliable{false};
  bool qsa_capture_skipped{false};
  std::uint64_t ple_reads{0};
  double ple_disk_ms{0};
  double ple_wait_ms{0};
  double ple_avg_wait_ms{0};
  double ple_max_wait_ms{0};
  std::uint64_t ple_wait_calls{0};
  std::string tensile_libpath;
  std::uint64_t lt_calls{0};
  std::uint64_t lt_success{0};
  std::uint64_t hipblas_fallback{0};
  std::uint64_t hipblas_direct{0};
  double fallback_pct{0};
  std::vector<ShapeCount> fallback_shapes;
  std::uint64_t stream_syncs{0};
  std::uint64_t device_syncs{0};
  std::uint64_t event_syncs{0};
  double sync_ms{0};
  StageRow stages[kStageCount]{};
};

struct Part {
  double wall{0};
  double ple{0};
  double sync{0};
  double qsa{0};
};

struct ShapeKey {
  int type{0};
  int m{0};
  int n{0};
  int k{0};
  int kind{0};
  bool operator==(const ShapeKey& o) const {
    return type == o.type && m == o.m && n == o.n && k == o.k && kind == o.kind;
  }
};

struct ShapeHash {
  std::size_t operator()(const ShapeKey& s) const noexcept {
    std::size_t h = static_cast<std::size_t>(s.type) * 1315423911u;
    h ^= static_cast<std::size_t>(s.m) * 2654435761u;
    h ^= static_cast<std::size_t>(s.n) * 2246822519u;
    h ^= static_cast<std::size_t>(s.k) * 3266489917u;
    h ^= static_cast<std::size_t>(s.kind) * 668265263u;
    return h;
  }
};

struct State {
  std::atomic<int> enabled{0};
  std::atomic<int> phase{0};
  std::atomic<std::int64_t> disk_ns{0};
  std::atomic<std::int64_t> disk_reads{0};

  double ttft_ms{0};
  std::uint32_t context_capacity{0};
  std::uint32_t context_depth{0};
  std::uint32_t draft_max{0};
  std::uint64_t graph_captures{0};

  double verify_trunk_ms{0};
  double verify_hc_head_ms{0};
  double verify_vocab_projection_ms{0};
  double verify_compact_select_ms{0};
  double verify_d2h_ms{0};
  double verify_cpu_accept_residual_ms{0};
  bool verify_gpu_saw_event{false};
  bool verify_gpu_incomplete{false};
  std::uint64_t verify_output_rows{0};
  std::uint64_t verify_full_rows_d2h{0};
  std::uint64_t verify_compact_rows_d2h{0};
  std::uint64_t verify_d2h_bytes{0};
  std::uint64_t verify_compact_candidates{0};
  std::uint64_t verify_certificate_successes{0};
  std::uint64_t verify_certificate_fallbacks{0};
  std::uint64_t verify_unsupported_sampler_fallbacks{0};
  std::uint64_t verify_rejection_frontier_downloads{0};
  std::uint64_t mtp_cycles{0};
  std::uint64_t mtp_drafted{0};
  std::uint64_t mtp_accepted{0};
  std::uint64_t lookup_cycles{0};
  std::uint64_t lookup_drafted{0};
  std::uint64_t lookup_accepted{0};
  std::uint64_t proposed_per_position[kDepths - 1]{};
  std::uint64_t accepted_per_position[kDepths - 1]{};
  std::uint64_t mtp_proposed_per_position[kDepths - 1]{};
  std::uint64_t mtp_accepted_per_position[kDepths - 1]{};
  std::uint64_t lookup_proposed_per_position[kDepths - 1]{};
  std::uint64_t lookup_accepted_per_position[kDepths - 1]{};

  double catchup_ms{0};
  double proposal_ms{0};
  double verify_forward_ms{0};
  double plain_forward_ms{0};
  double decision_ms{0};
  double rollback_ms{0};
  double sampling_ms{0};
  Part part_sampling{};
  Part part_catchup{};
  Part part_proposal{};
  Part part_verify{};
  Part part_plain{};
  Part part_decision{};
  Part part_rollback{};

  double ple_wait_ms{0};
  double ple_wait_max_ms{0};
  std::uint64_t ple_wait_calls{0};

  double qsa_indexer{0};
  double qsa_selector{0};
  double qsa_sparse{0};
  bool qsa_failed{false};
  bool qsa_truncated{false};
  bool qsa_saw_event{false};
  bool qsa_capture_skipped{false};

  std::uint64_t stream_syncs{0};
  std::uint64_t device_syncs{0};
  std::uint64_t event_syncs{0};
  double sync_ms{0};

  std::uint64_t lt_calls{0};
  std::uint64_t lt_success{0};
  std::uint64_t hipblas_fallback{0};
  std::uint64_t hipblas_direct{0};
  std::unordered_map<ShapeKey, std::uint64_t, ShapeHash> shapes;

  DepthRow depths[kDepths]{};
  DepthRow mtp_depths[kDepths]{};
  DepthRow lookup_depths[kDepths]{};
  std::vector<CycleRec> cycles;
  bool cycle_open{false};
  double cycle_t0{0};
  double snap_proposal{0};
  double snap_verify{0};
  double snap_qsa{0};
  double snap_ple{0};
};

inline State& S() {
  static State state;
  return state;
}

inline void (*OnReset)() = nullptr;

inline bool Enabled() noexcept {
  return S().enabled.load(std::memory_order_relaxed) != 0;
}

inline Phase CurrentPhase() noexcept {
  return static_cast<Phase>(S().phase.load(std::memory_order_relaxed));
}

inline bool OnDecode() noexcept {
  return Enabled() && CurrentPhase() == Phase::Decode;
}

inline double NowMs() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch())
      .count();
}

inline double QsaTotal(const State& s) {
  return s.qsa_indexer + s.qsa_selector + s.qsa_sparse;
}

inline void SetEnabled(bool on) noexcept {
  S().enabled.store(on ? 1 : 0, std::memory_order_relaxed);
}

inline void SetPhase(Phase phase) noexcept {
  S().phase.store(static_cast<int>(phase), std::memory_order_relaxed);
}

inline void Reset() {
  auto& s = S();
  s.ttft_ms = 0;
  s.context_capacity = 0;
  s.context_depth = 0;
  s.draft_max = 0;
  s.graph_captures = 0;
  s.verify_trunk_ms = s.verify_hc_head_ms = 0;
  s.verify_vocab_projection_ms = s.verify_compact_select_ms = 0;
  s.verify_d2h_ms = s.verify_cpu_accept_residual_ms = 0;
  s.verify_gpu_saw_event = false;
  s.verify_gpu_incomplete = false;
  s.verify_output_rows = 0;
  s.verify_full_rows_d2h = 0;
  s.verify_compact_rows_d2h = 0;
  s.verify_d2h_bytes = 0;
  s.verify_compact_candidates = 0;
  s.verify_certificate_successes = 0;
  s.verify_certificate_fallbacks = 0;
  s.verify_unsupported_sampler_fallbacks = 0;
  s.verify_rejection_frontier_downloads = 0;
  s.mtp_cycles = s.mtp_drafted = s.mtp_accepted = 0;
  s.lookup_cycles = s.lookup_drafted = s.lookup_accepted = 0;
  std::fill(std::begin(s.proposed_per_position), std::end(s.proposed_per_position), 0);
  std::fill(std::begin(s.accepted_per_position), std::end(s.accepted_per_position), 0);
  std::fill(std::begin(s.mtp_proposed_per_position), std::end(s.mtp_proposed_per_position), 0);
  std::fill(std::begin(s.mtp_accepted_per_position), std::end(s.mtp_accepted_per_position), 0);
  std::fill(std::begin(s.lookup_proposed_per_position), std::end(s.lookup_proposed_per_position), 0);
  std::fill(std::begin(s.lookup_accepted_per_position), std::end(s.lookup_accepted_per_position), 0);
  s.catchup_ms = s.proposal_ms = s.verify_forward_ms = s.plain_forward_ms = 0;
  s.decision_ms = s.rollback_ms = s.sampling_ms = 0;
  s.part_sampling = s.part_catchup = s.part_proposal = {};
  s.part_verify = s.part_plain = s.part_decision = s.part_rollback = {};
  s.ple_wait_ms = 0;
  s.ple_wait_max_ms = 0;
  s.ple_wait_calls = 0;
  s.qsa_indexer = s.qsa_selector = s.qsa_sparse = 0;
  s.qsa_failed = s.qsa_truncated = s.qsa_saw_event = s.qsa_capture_skipped = false;
  s.stream_syncs = s.device_syncs = s.event_syncs = 0;
  s.sync_ms = 0;
  s.lt_calls = s.lt_success = s.hipblas_fallback = s.hipblas_direct = 0;
  s.shapes.clear();
  for (auto& d : s.depths) d = {};
  for (auto& d : s.mtp_depths) d = {};
  for (auto& d : s.lookup_depths) d = {};
  s.cycles.clear();
  s.cycle_open = false;
  s.disk_ns.store(0, std::memory_order_relaxed);
  s.disk_reads.store(0, std::memory_order_relaxed);
  if (OnReset != nullptr) OnReset();
}

inline void SetTtft(double ms) { if (Enabled()) S().ttft_ms = ms; }
inline void SetContext(std::uint32_t capacity, std::uint32_t depth) {
  if (!Enabled()) return;
  S().context_capacity = capacity;
  S().context_depth = depth;
}
inline void SetDraftMax(std::uint32_t draft_max) {
  if (Enabled()) S().draft_max = draft_max;
}
inline void NoteGraphCapture() {
  if (OnDecode()) ++S().graph_captures;
}

inline void AddVerifySubstage(VerifySubstage stage, double ms) {
  if (!OnDecode() || !(ms >= 0.0)) return;
  auto& s = S();
  switch (stage) {
    case VerifySubstage::Trunk: s.verify_trunk_ms += ms; break;
    case VerifySubstage::HcHead: s.verify_hc_head_ms += ms; break;
    case VerifySubstage::VocabProjection: s.verify_vocab_projection_ms += ms; break;
    case VerifySubstage::CompactSelect: s.verify_compact_select_ms += ms; break;
    case VerifySubstage::D2H: s.verify_d2h_ms += ms; break;
    case VerifySubstage::CpuAcceptResidual:
      s.verify_cpu_accept_residual_ms += ms;
      break;
  }
}

inline void NoteVerifyGpuEvent() {
  if (OnDecode()) S().verify_gpu_saw_event = true;
}
inline void SetVerifyGpuEventsIncomplete() {
  if (Enabled()) S().verify_gpu_incomplete = true;
}
inline void NoteVerifyOutputRows(std::uint32_t rows) {
  if (OnDecode()) S().verify_output_rows += rows;
}
inline void AddVerificationTransfer(std::uint64_t full_rows,
                                    std::uint64_t compact_rows,
                                    std::uint64_t bytes,
                                    std::uint64_t compact_candidates) {
  if (!OnDecode()) return;
  auto& s = S();
  s.verify_full_rows_d2h += full_rows;
  s.verify_compact_rows_d2h += compact_rows;
  s.verify_d2h_bytes += bytes;
  s.verify_compact_candidates += compact_candidates;
}
inline void NoteCertificateSuccess() {
  if (OnDecode()) ++S().verify_certificate_successes;
}
inline void NoteCertificateFallback() {
  if (OnDecode()) ++S().verify_certificate_fallbacks;
}
inline void NoteUnsupportedSamplerFallback() {
  if (OnDecode()) ++S().verify_unsupported_sampler_fallbacks;
}
inline void NoteRejectionFrontierDownload() {
  if (OnDecode()) ++S().verify_rejection_frontier_downloads;
}

struct VerifyCpuScope {
  bool on{false};
  double t0{0};
  VerifyCpuScope() {
    if (OnDecode()) {
      on = true;
      t0 = NowMs();
    }
  }
  ~VerifyCpuScope() {
    if (on) AddVerifySubstage(VerifySubstage::CpuAcceptResidual, NowMs() - t0);
  }
  VerifyCpuScope(const VerifyCpuScope&) = delete;
  VerifyCpuScope& operator=(const VerifyCpuScope&) = delete;
};

inline void AddPleWait(double ms) {
  if (!OnDecode()) return;
  auto& s = S();
  s.ple_wait_ms += ms;
  ++s.ple_wait_calls;
  if (ms > s.ple_wait_max_ms) s.ple_wait_max_ms = ms;
}

inline void AddDiskRead(double ms) {
  if (!OnDecode()) return;
  auto& s = S();
  s.disk_ns.fetch_add(static_cast<std::int64_t>(ms * 1.0e6),
                      std::memory_order_relaxed);
  s.disk_reads.fetch_add(1, std::memory_order_relaxed);
}

inline void AddQsa(int kind, double ms) {
  if (!OnDecode() || !(ms >= 0)) return;
  auto& s = S();
  s.qsa_saw_event = true;
  if (kind == 0) s.qsa_indexer += ms;
  else if (kind == 1) s.qsa_selector += ms;
  else s.qsa_sparse += ms;
}

inline void SetQsaFailed() { if (Enabled()) S().qsa_failed = true; }
inline void SetQsaTruncated() { if (Enabled()) S().qsa_truncated = true; }
inline void SetQsaCaptureSkipped() { if (Enabled()) S().qsa_capture_skipped = true; }

inline void AddStreamSync(double ms) {
  if (!OnDecode()) return;
  auto& s = S();
  s.sync_ms += ms;
  ++s.stream_syncs;
}
inline void AddDeviceSync(double ms) {
  if (!OnDecode()) return;
  auto& s = S();
  s.sync_ms += ms;
  ++s.device_syncs;
}
inline void AddEventSync(double ms) {
  if (!OnDecode()) return;
  auto& s = S();
  s.sync_ms += ms;
  ++s.event_syncs;
}

template <class Stream, class Fn>
inline auto TimedStreamSync(Stream stream, Fn sync) -> decltype(sync(stream)) {
  if (!OnDecode()) return sync(stream);
  const double t0 = NowMs();
  auto err = sync(stream);
  AddStreamSync(NowMs() - t0);
  return err;
}

template <class Fn>
inline auto TimedDeviceSync(Fn sync) -> decltype(sync()) {
  if (!OnDecode()) return sync();
  const double t0 = NowMs();
  auto err = sync();
  AddDeviceSync(NowMs() - t0);
  return err;
}

template <class Event, class Fn>
inline auto TimedEventSync(Event event, Fn sync) -> decltype(sync(event)) {
  if (!OnDecode()) return sync(event);
  const double t0 = NowMs();
  auto err = sync(event);
  AddEventSync(NowMs() - t0);
  return err;
}

inline void LtCall() { if (Enabled()) ++S().lt_calls; }
inline void LtOk() { if (Enabled()) ++S().lt_success; }

inline void NoteGemm(int kind, int type, int m, int n, int k) {
  if (!Enabled()) return;
  auto& s = S();
  if (kind == 0) ++s.hipblas_fallback;
  else ++s.hipblas_direct;
  ShapeKey key{type, m, n, k, kind};
  ++s.shapes[key];
}

struct Scope {
  Stage stage{Stage::Sampling};
  bool on{false};
  double t0{0};
  double ple0{0};
  double sync0{0};
  double qsa0{0};

  explicit Scope(Stage stage_in) : stage(stage_in) {
    if (!OnDecode()) return;
    on = true;
    auto& s = S();
    t0 = NowMs();
    ple0 = s.ple_wait_ms;
    sync0 = s.sync_ms;
    qsa0 = QsaTotal(s);
  }

  ~Scope() {
    if (!on) return;
    auto& s = S();
    Part part;
    part.wall = NowMs() - t0;
    part.ple = s.ple_wait_ms - ple0;
    part.sync = s.sync_ms - sync0;
    part.qsa = QsaTotal(s) - qsa0;
    Part* dest = &s.part_sampling;
    double* raw = &s.sampling_ms;
    switch (stage) {
      case Stage::Sampling: dest = &s.part_sampling; raw = &s.sampling_ms; break;
      case Stage::Catchup: dest = &s.part_catchup; raw = &s.catchup_ms; break;
      case Stage::Proposal: dest = &s.part_proposal; raw = &s.proposal_ms; break;
      case Stage::VerifyForward: dest = &s.part_verify; raw = &s.verify_forward_ms; break;
      case Stage::PlainForward: dest = &s.part_plain; raw = &s.plain_forward_ms; break;
      case Stage::Decision: dest = &s.part_decision; raw = &s.decision_ms; break;
      case Stage::Rollback: dest = &s.part_rollback; raw = &s.rollback_ms; break;
    }
    dest->wall += part.wall;
    dest->ple += part.ple;
    dest->sync += part.sync;
    dest->qsa += part.qsa;
    *raw += part.wall;
  }

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

inline void BeginCycle() {
  if (!OnDecode()) return;
  auto& s = S();
  s.cycle_open = true;
  s.cycle_t0 = NowMs();
  s.snap_proposal = s.catchup_ms + s.proposal_ms;
  s.snap_verify = s.verify_forward_ms;
  s.snap_qsa = QsaTotal(s);
  s.snap_ple = s.ple_wait_ms;
}

inline void CancelCycle() { S().cycle_open = false; }

inline void EndCycle(std::uint32_t depth, std::uint32_t drafted,
                     std::uint32_t accepted, std::uint32_t output_tokens,
                     ProposalSource source = ProposalSource::Mtp) {
  auto& s = S();
  if (!s.cycle_open) return;
  s.cycle_open = false;
  CycleRec cycle;
  cycle.output_tokens = output_tokens;
  cycle.drafted = drafted;
  cycle.accepted = accepted;
  cycle.depth = std::min<std::uint32_t>(depth, kDepths - 1);
  cycle.cycle_ms = NowMs() - s.cycle_t0;
  cycle.proposal_ms = (s.catchup_ms + s.proposal_ms) - s.snap_proposal;
  cycle.verify_ms = s.verify_forward_ms - s.snap_verify;
  cycle.qsa_ms = QsaTotal(s) - s.snap_qsa;
  cycle.ple_ms = s.ple_wait_ms - s.snap_ple;
  cycle.source = source;
  s.cycles.push_back(cycle);
  if (source == ProposalSource::Mtp) {
    ++s.mtp_cycles;
    s.mtp_drafted += drafted;
    s.mtp_accepted += accepted;
  } else if (source == ProposalSource::Lookup) {
    ++s.lookup_cycles;
    s.lookup_drafted += drafted;
    s.lookup_accepted += accepted;
  }
  for (std::uint32_t position = 0;
       position < std::min<std::uint32_t>(drafted, kDepths - 1); ++position) {
    ++s.proposed_per_position[position];
    if (accepted > position) ++s.accepted_per_position[position];
    if (source == ProposalSource::Mtp) {
      ++s.mtp_proposed_per_position[position];
      if (accepted > position) ++s.mtp_accepted_per_position[position];
    } else if (source == ProposalSource::Lookup) {
      ++s.lookup_proposed_per_position[position];
      if (accepted > position) ++s.lookup_accepted_per_position[position];
    }
  }
  auto& row = s.depths[cycle.depth];
  ++row.cycles;
  row.cycle_ms += cycle.cycle_ms;
  row.drafted += drafted;
  row.accepted += accepted;
  row.output_tokens += output_tokens;
  row.proposal_ms += cycle.proposal_ms;
  row.verify_ms += cycle.verify_ms;
  if (source == ProposalSource::Mtp) {
    auto& source_row = s.mtp_depths[cycle.depth];
    ++source_row.cycles;
    source_row.cycle_ms += cycle.cycle_ms;
    source_row.drafted += drafted;
    source_row.accepted += accepted;
    source_row.output_tokens += output_tokens;
    source_row.proposal_ms += cycle.proposal_ms;
    source_row.verify_ms += cycle.verify_ms;
  } else if (source == ProposalSource::Lookup) {
    auto& source_row = s.lookup_depths[cycle.depth];
    ++source_row.cycles;
    source_row.cycle_ms += cycle.cycle_ms;
    source_row.drafted += drafted;
    source_row.accepted += accepted;
    source_row.output_tokens += output_tokens;
    source_row.proposal_ms += cycle.proposal_ms;
    source_row.verify_ms += cycle.verify_ms;
  }
}

inline double Exclusive(const Part& part) {
  return std::max(0.0, part.wall - part.ple - part.sync);
}

inline Report BuildReport(double decode_ms) {
  const auto& s = S();
  Report report;
  report.present = true;
  report.ttft_ms = s.ttft_ms;
  report.context_capacity = s.context_capacity;
  report.context_depth = s.context_depth;
  report.draft_max = s.draft_max;
  report.graph_captures = s.graph_captures;
  report.verify_trunk_ms = s.verify_trunk_ms;
  report.verify_hc_head_ms = s.verify_hc_head_ms;
  report.verify_vocab_projection_ms = s.verify_vocab_projection_ms;
  report.verify_compact_select_ms = s.verify_compact_select_ms;
  report.verify_d2h_ms = s.verify_d2h_ms;
  report.verify_cpu_accept_residual_ms = s.verify_cpu_accept_residual_ms;
  report.verify_gpu_events_reliable =
      s.verify_gpu_saw_event && !s.verify_gpu_incomplete;
  report.verify_gpu_events_incomplete = s.verify_gpu_incomplete;
  report.verify_output_rows = s.verify_output_rows;
  report.verify_full_rows_d2h = s.verify_full_rows_d2h;
  report.verify_compact_rows_d2h = s.verify_compact_rows_d2h;
  report.verify_d2h_bytes = s.verify_d2h_bytes;
  report.verify_compact_candidates = s.verify_compact_candidates;
  report.verify_certificate_successes = s.verify_certificate_successes;
  report.verify_certificate_fallbacks = s.verify_certificate_fallbacks;
  report.verify_unsupported_sampler_fallbacks =
      s.verify_unsupported_sampler_fallbacks;
  report.verify_rejection_frontier_downloads =
      s.verify_rejection_frontier_downloads;
  report.mtp_cycles = s.mtp_cycles;
  report.mtp_drafted = s.mtp_drafted;
  report.mtp_accepted = s.mtp_accepted;
  report.lookup_cycles = s.lookup_cycles;
  report.lookup_drafted = s.lookup_drafted;
  report.lookup_accepted = s.lookup_accepted;
  std::copy(std::begin(s.proposed_per_position), std::end(s.proposed_per_position),
            std::begin(report.proposed_per_position));
  std::copy(std::begin(s.accepted_per_position), std::end(s.accepted_per_position),
            std::begin(report.accepted_per_position));
  std::copy(std::begin(s.mtp_proposed_per_position), std::end(s.mtp_proposed_per_position),
            std::begin(report.mtp_proposed_per_position));
  std::copy(std::begin(s.mtp_accepted_per_position), std::end(s.mtp_accepted_per_position),
            std::begin(report.mtp_accepted_per_position));
  std::copy(std::begin(s.lookup_proposed_per_position), std::end(s.lookup_proposed_per_position),
            std::begin(report.lookup_proposed_per_position));
  std::copy(std::begin(s.lookup_accepted_per_position), std::end(s.lookup_accepted_per_position),
            std::begin(report.lookup_accepted_per_position));
  report.catchup_ms = s.catchup_ms;
  report.proposal_ms = s.proposal_ms;
  report.verification_ms = s.verify_forward_ms;
  report.rollback_ms = s.rollback_ms;
  report.mtp_cycle_ms = s.catchup_ms + s.proposal_ms + s.verify_forward_ms +
                        s.decision_ms + s.rollback_ms;
  for (const auto& cycle : s.cycles) {
    if (cycle.source == ProposalSource::Mtp) {
      report.mtp_output_tokens += cycle.output_tokens;
      report.mtp_proposal_ms += cycle.proposal_ms;
      report.mtp_verification_ms += cycle.verify_ms;
      report.mtp_total_cycle_ms += cycle.cycle_ms;
    } else if (cycle.source == ProposalSource::Lookup) {
      report.lookup_output_tokens += cycle.output_tokens;
      report.lookup_proposal_ms += cycle.proposal_ms;
      report.lookup_verification_ms += cycle.verify_ms;
      report.lookup_total_cycle_ms += cycle.cycle_ms;
    }
  }
  for (int i = 0; i < kDepths; ++i) {
    report.depths[i] = s.depths[i];
    report.mtp_depths[i] = s.mtp_depths[i];
    report.lookup_depths[i] = s.lookup_depths[i];
  }
  double depth_sum = 0;
  std::uint64_t depth_cycles = 0;
  for (int i = 0; i < kDepths; ++i) {
    depth_sum += static_cast<double>(i) * static_cast<double>(s.depths[i].cycles);
    depth_cycles += s.depths[i].cycles;
  }
  report.avg_draft_depth = depth_cycles ? depth_sum / static_cast<double>(depth_cycles) : 0;
  double mtp_depth_sum = 0;
  double lookup_depth_sum = 0;
  std::uint64_t mtp_depth_cycles = 0;
  std::uint64_t lookup_depth_cycles = 0;
  for (int i = 0; i < kDepths; ++i) {
    mtp_depth_sum += static_cast<double>(i) * static_cast<double>(s.mtp_depths[i].cycles);
    mtp_depth_cycles += s.mtp_depths[i].cycles;
    lookup_depth_sum += static_cast<double>(i) * static_cast<double>(s.lookup_depths[i].cycles);
    lookup_depth_cycles += s.lookup_depths[i].cycles;
  }
  report.mtp_avg_draft_depth = mtp_depth_cycles ? mtp_depth_sum / static_cast<double>(mtp_depth_cycles) : 0;
  report.lookup_avg_draft_depth = lookup_depth_cycles ? lookup_depth_sum / static_cast<double>(lookup_depth_cycles) : 0;

  std::uint32_t cursor = 0;
  double wall = 0;
  double proposal = 0;
  double verify = 0;
  double qsa = 0;
  double ple = 0;
  double depth_acc = 0;
  std::uint32_t cycles = 0;
  std::uint64_t drafted = 0;
  std::uint64_t accepted = 0;
  std::uint32_t tokens = 0;
  auto flush = [&] {
    if (tokens == 0) return;
    WindowRow row;
    row.token_lo = cursor - tokens + 1;
    row.token_hi = cursor;
    row.tokens_per_second = wall > 0 ? 1000.0 * tokens / wall : 0;
    row.acceptance = drafted ? static_cast<double>(accepted) / static_cast<double>(drafted) : 0;
    row.avg_draft_depth = cycles ? depth_acc / static_cast<double>(cycles) : 0;
    row.proposal_ms = proposal;
    row.verify_ms = verify;
    row.qsa_ms = qsa;
    row.ple_wait_ms = ple;
    report.windows.push_back(row);
    wall = proposal = verify = qsa = ple = depth_acc = 0;
    cycles = 0;
    drafted = accepted = 0;
    tokens = 0;
  };
  for (const CycleRec& cycle : s.cycles) {
    if (cycle.output_tokens == 0) continue;
    cursor += cycle.output_tokens;
    tokens += cycle.output_tokens;
    wall += cycle.cycle_ms;
    proposal += cycle.proposal_ms;
    verify += cycle.verify_ms;
    qsa += cycle.qsa_ms;
    ple += cycle.ple_ms;
    depth_acc += cycle.depth;
    ++cycles;
    drafted += cycle.drafted;
    accepted += cycle.accepted;
    if (tokens >= 64) flush();
  }
  flush();

  report.qsa_indexer_ms = s.qsa_indexer;
  report.qsa_selector_ms = s.qsa_selector;
  report.qsa_sparse_ms = s.qsa_sparse;
  report.qsa_total_ms = QsaTotal(s);
  report.qsa_events_reliable = s.qsa_saw_event && !s.qsa_failed && !s.qsa_truncated &&
                               !s.qsa_capture_skipped;
  report.qsa_capture_skipped = s.qsa_capture_skipped;
  report.ple_reads = static_cast<std::uint64_t>(s.disk_reads.load(std::memory_order_relaxed));
  report.ple_disk_ms =
      static_cast<double>(s.disk_ns.load(std::memory_order_relaxed)) / 1.0e6;
  report.ple_wait_ms = s.ple_wait_ms;
  report.ple_wait_calls = s.ple_wait_calls;
  report.ple_avg_wait_ms =
      s.ple_wait_calls ? s.ple_wait_ms / static_cast<double>(s.ple_wait_calls) : 0;
  report.ple_max_wait_ms = s.ple_wait_max_ms;
  char* tensile = nullptr;
  std::size_t tensile_len = 0;
  if (_dupenv_s(&tensile, &tensile_len, "HIPBLASLT_TENSILE_LIBPATH") == 0 &&
      tensile != nullptr) {
    report.tensile_libpath.assign(tensile, tensile_len ? tensile_len - 1 : 0);
    std::free(tensile);
  }
  report.lt_calls = s.lt_calls;
  report.lt_success = s.lt_success;
  report.hipblas_fallback = s.hipblas_fallback;
  report.hipblas_direct = s.hipblas_direct;
  const double blas_den =
      static_cast<double>(s.lt_success + s.hipblas_fallback);
  report.fallback_pct = blas_den > 0 ? 100.0 * static_cast<double>(s.hipblas_fallback) / blas_den : 0;
  report.fallback_shapes.reserve(s.shapes.size());
  for (const auto& [key, count] : s.shapes) {
    ShapeCount shape;
    shape.type = key.type;
    shape.m = key.m;
    shape.n = key.n;
    shape.k = key.k;
    shape.kind = key.kind;
    shape.count = count;
    report.fallback_shapes.push_back(shape);
  }
  std::sort(report.fallback_shapes.begin(), report.fallback_shapes.end(),
            [](const ShapeCount& a, const ShapeCount& b) { return a.count > b.count; });
  if (report.fallback_shapes.size() > 8) report.fallback_shapes.resize(8);
  report.stream_syncs = s.stream_syncs;
  report.device_syncs = s.device_syncs;
  report.event_syncs = s.event_syncs;
  report.sync_ms = s.sync_ms;

  const double qsa_accounted = std::min(report.qsa_total_ms, s.sync_ms);
  const double sync_accounted = std::max(0.0, s.sync_ms - qsa_accounted);
  const double target = Exclusive(s.part_plain);
  const double proposal_ex = Exclusive(s.part_catchup) + Exclusive(s.part_proposal);
  const double verify_ex = Exclusive(s.part_verify) + Exclusive(s.part_decision);
  const double rollback_ex = Exclusive(s.part_rollback);
  const double sampling = s.sampling_ms;
  const double named = target + proposal_ex + verify_ex + qsa_accounted + s.ple_wait_ms +
                       rollback_ex + sampling + sync_accounted;
  const double other = std::max(0.0, decode_ms - named);
  const double rows[kStageCount] = {target,     proposal_ex, verify_ex, qsa_accounted,
                                    s.ple_wait_ms, rollback_ex, sampling, sync_accounted,
                                    other};
  const char* names[kStageCount] = {
      "Target/trunk", "Spec proposal", "Spec verification", "QSA",
      "PLE wait",     "Rollback",      "Sampling",         "Synchronization",
      "Other"};
  for (int i = 0; i < kStageCount; ++i) {
    report.stages[i].name = names[i];
    report.stages[i].ms = rows[i];
    report.stages[i].pct = decode_ms > 0 ? 100.0 * rows[i] / decode_ms : 0;
  }
  return report;
}

}  // namespace fnvprof
