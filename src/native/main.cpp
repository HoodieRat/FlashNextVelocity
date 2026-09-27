#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "src/core/platform/device_memory.hpp"
#include "src/core/image.hpp"
#include "src/core/json.hpp"
#include "src/core/sampling.hpp"
#include "src/core/session_mode.hpp"
#include "src/models/qwen/chat_template.hpp"
#include "src/models/qwen/vision/prompt.hpp"
#include "src/models/qwen38_flash_next/engine.hpp"
#include "bench_profile.hpp"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using qfn = gufo::models::qwen38_flash_next::Model;
using QfnSession = gufo::models::qwen38_flash_next::Session;
using gufo::json::Value;

namespace {

constexpr std::string_view kRuntimeRevision = "fnv-async-halo-pipeline-v12";

struct Config {
  std::string model;
  std::string mtp;
  std::string mmproj;
  std::string host{"127.0.0.1"};
  int port{8080};
  std::uint32_t context{131072};
  std::uint32_t draft_max{7};
  float draft_confidence{0.0F};
  std::string mtp_proposal_mode{"halo_greedy"};
  std::uint32_t prefill_batch{2048};
  bool context_lookup{true};
  std::uint32_t context_lookup_min_ngram{3};
  std::uint32_t context_lookup_max_ngram{6};
  std::uint32_t context_lookup_window{32768};
  std::uint32_t context_lookup_min_draft{2};
  bool memory_guard{true};
  double memory_guard_min_available_gib{8.0};
  std::uint32_t sessions{1};
  std::size_t default_max_tokens{4096};
  bool thinking{true};
  bool preserve_thinking{false};
  std::string reasoning_effort{"medium"};
  gufo::sampling::SamplingConfig sampling{
      .temperature = 0.35F,
      .top_k = 40,
      .top_p = 0.90F,
      .min_p = 0.05F,
      .min_keep = 1,
      .seed = -1,
      .repeat_penalty = 1.05F,
      .repeat_last_n = 512,
      .frequency_penalty = 0.0F,
      .presence_penalty = 0.0F,
  };
};

struct Metrics {
  std::uint64_t prompt_tokens{0};
  std::uint64_t completion_tokens{0};
  double prefill_ms{0};
  double decode_ms{0};
  double prefill_tps{0};
  double decode_tps{0};
  std::uint64_t draft_tokens{0};
  std::uint64_t draft_accepted{0};
  double draft_acceptance{0};
  double ttft_ms{0};
  double avg_inter_token_ms{0};
  std::uint32_t context_capacity{0};
  std::uint32_t context_depth{0};
  std::uint32_t draft_max{0};
  double avg_draft_depth{0};
  double draft_confidence{0};
  std::string mtp_proposal_mode{"halo_greedy"};
  std::uint32_t prefill_batch{2048};
  bool thinking{false};
  bool preserve_thinking{false};
  std::string reasoning_effort{"OFF"};
  std::uint32_t compact_verification_candidates{0};
  double temperature{0};
  double top_p{0};
  std::int32_t top_k{0};
  double min_p{0};
  double repeat_penalty{1};
  std::size_t repeat_last_n{0};
  std::uint64_t low_confidence_stops{0};
  std::uint64_t compact_frontier_samples{0};
  std::uint64_t compact_frontier_fallbacks{0};
  std::uint64_t retained_frontier_downloads{0};
  std::uint64_t async_halo_chains{0};
  std::uint64_t async_halo_draft_tokens{0};
  std::uint64_t mtp_fresh_resets{0};
  std::uint64_t mtp_drafted{0};
  std::uint64_t mtp_accepted{0};
  double mtp_avg_proposal_confidence{0};
  std::uint64_t lookup_cycles{0};
  std::uint64_t lookup_drafted{0};
  std::uint64_t lookup_accepted{0};
  bool context_lookup_enabled{true};
  double lookup_avg_ngram{0};
  double lookup_avg_distance{0};
  std::uint64_t memory_available_bytes{0};
  std::uint64_t memory_commit_available_bytes{0};
  struct Window {
    std::uint32_t token_lo{0};
    std::uint32_t token_hi{0};
    double tokens_per_second{0};
    double acceptance{0};
    double avg_draft_depth{0};
  };
  std::vector<Window> request_windows;
  std::vector<double> speed_windows;
  bool has_request{false};
  fnvprof::Report profile{};
};

struct ToolCall {
  std::string id;
  std::string name;
  Value arguments{Value::object()};
};

struct ParsedOutput {
  std::string content;
  std::string reasoning;
  std::vector<ToolCall> calls;
};

struct ChatInput {
  std::vector<gufo::tokenization::ChatMessage> messages;
  std::vector<gufo::tokenization::ChatTool> tools;
  bool tool_required{false};
  bool tool_none{false};
  bool enable_thinking{true};
  bool preserve_thinking{false};
  gufo::tokenization::QwenReasoningEffort reasoning_effort{
      gufo::tokenization::QwenReasoningEffort::kMedium};
  bool add_vision_id{false};
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
};

struct HttpResponse {
  int status{200};
  std::string content_type{"application/json"};
  std::string body;
};

std::string Slurp(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open " + path.string());
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
}

std::string RandomId(std::string_view prefix) {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  static constexpr char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string out(prefix);
  for (int i = 0; i < 20; ++i) out.push_back(chars[rng() % (sizeof(chars)-1)]);
  return out;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string Trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return std::string(value);
}

std::size_t CompleteUtf8Prefix(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    std::size_t need = 1;
    if (c < 0x80) need = 1;
    else if ((c & 0xE0) == 0xC0) need = 2;
    else if ((c & 0xF0) == 0xE0) need = 3;
    else if ((c & 0xF8) == 0xF0) need = 4;
    else { ++i; continue; }
    if (i + need > text.size()) break;
    bool valid = true;
    for (std::size_t j = 1; j < need; ++j) {
      if ((static_cast<unsigned char>(text[i+j]) & 0xC0) != 0x80) { valid = false; break; }
    }
    if (!valid) { ++i; continue; }
    i += need;
  }
  return i;
}

Config LoadConfig(const fs::path& path) {
  Config c;
  Value root = gufo::json::parse(Slurp(path));
  if (!root.is_object()) throw std::runtime_error("config root must be an object");
  c.model = root.member_str("model");
  c.mtp = root.member_str("mtp");
  c.mmproj = root.member_str("mmproj");
  c.host = root.member_str("host", c.host);
  c.port = static_cast<int>(root.member_size("port", c.port));
  c.context = static_cast<std::uint32_t>(root.member_size("context", c.context));
  c.draft_max = static_cast<std::uint32_t>(root.member_size("draft_max", c.draft_max));
  c.draft_confidence = static_cast<float>(root.member_double("draft_confidence", c.draft_confidence));
  c.mtp_proposal_mode = root.member_str("mtp_proposal_mode", c.mtp_proposal_mode);
  c.prefill_batch = static_cast<std::uint32_t>(root.member_size("prefill_batch", c.prefill_batch));
  if (const auto* v = root.find("context_lookup"); v && v->is_bool()) c.context_lookup = v->as_bool();
  c.context_lookup_min_ngram = static_cast<std::uint32_t>(root.member_size("context_lookup_min_ngram", c.context_lookup_min_ngram));
  c.context_lookup_max_ngram = static_cast<std::uint32_t>(root.member_size("context_lookup_max_ngram", c.context_lookup_max_ngram));
  c.context_lookup_window = static_cast<std::uint32_t>(root.member_size("context_lookup_window", c.context_lookup_window));
  c.context_lookup_min_draft = static_cast<std::uint32_t>(root.member_size("context_lookup_min_draft", c.context_lookup_min_draft));
  if (const auto* v = root.find("memory_guard"); v && v->is_bool()) c.memory_guard = v->as_bool();
  c.memory_guard_min_available_gib = root.member_double("memory_guard_min_available_gib", c.memory_guard_min_available_gib);
  c.sessions = static_cast<std::uint32_t>(root.member_size("sessions", c.sessions));
  c.default_max_tokens = root.member_size("default_max_tokens", c.default_max_tokens);
  if (const auto* v = root.find("thinking"); v && v->is_bool()) c.thinking = v->as_bool();
  if (const auto* v = root.find("preserve_thinking"); v && v->is_bool()) c.preserve_thinking = v->as_bool();
  c.reasoning_effort = root.member_str("reasoning_effort", c.reasoning_effort);
  if (const auto* s = root.find("sampling"); s && s->is_object()) {
    c.sampling.temperature = static_cast<float>(s->member_double("temperature", c.sampling.temperature));
    c.sampling.top_k = static_cast<std::int32_t>(s->member_size("top_k", c.sampling.top_k));
    c.sampling.top_p = static_cast<float>(s->member_double("top_p", c.sampling.top_p));
    c.sampling.min_p = static_cast<float>(s->member_double("min_p", c.sampling.min_p));
    c.sampling.repeat_penalty = static_cast<float>(s->member_double("repeat_penalty", c.sampling.repeat_penalty));
    c.sampling.repeat_last_n = s->member_size("repeat_last_n", c.sampling.repeat_last_n);
  }
  c.sampling.Validate();
  if (c.model.empty()) throw std::runtime_error("config.model is required");
  if (c.context == 0 || c.draft_max == 0 || c.draft_max > 7)
    throw std::runtime_error("context/draft_max is invalid");
  if (c.draft_confidence < 0.0F || c.draft_confidence > 1.0F)
    throw std::runtime_error("draft_confidence must be from 0 to 1");
  if (c.mtp_proposal_mode != "halo_greedy" &&
      c.mtp_proposal_mode != "distribution")
    throw std::runtime_error("mtp_proposal_mode must be halo_greedy or distribution");
  if (c.prefill_batch < 128 || c.prefill_batch > 4096)
    throw std::runtime_error("prefill_batch must be from 128 to 4096");
  if (c.context_lookup &&
      (c.context_lookup_min_ngram < 2 ||
       c.context_lookup_max_ngram < c.context_lookup_min_ngram ||
       c.context_lookup_max_ngram > 32 || c.context_lookup_min_draft == 0 ||
       c.context_lookup_min_draft > gufo::models::qwen38_flash_next::kMaxMtpDraftTokens))
    throw std::runtime_error("context lookup settings are invalid");
  if (!std::isfinite(c.memory_guard_min_available_gib) ||
      c.memory_guard_min_available_gib < 0.0 ||
      c.memory_guard_min_available_gib > 128.0)
    throw std::runtime_error("memory_guard_min_available_gib is invalid");
  if (c.sessions != 1)
    throw std::runtime_error("this Windows server currently supports sessions=1; Gufo batching remains in the engine for a later scheduler layer");
  return c;
}

gufo::models::qwen38_flash_next::SampledMtpProposalMode ParseMtpProposalMode(
    std::string_view value) {
  using Mode = gufo::models::qwen38_flash_next::SampledMtpProposalMode;
  return value == "distribution" ? Mode::kDistribution : Mode::kHaloGreedy;
}

std::string_view MtpProposalModeDisplay(std::string_view value) {
  return value == "distribution" ? "distribution (Gufo p/q)"
                                 : "halo-greedy (old-runtime compatible)";
}

gufo::tokenization::QwenReasoningEffort ParseEffort(std::string_view value) {
  if (value == "low" || value == "minimal")
    return gufo::tokenization::QwenReasoningEffort::kLow;
  if (value == "medium")
    return gufo::tokenization::QwenReasoningEffort::kMedium;
  return gufo::tokenization::QwenReasoningEffort::kXHigh;
}

std::string_view EffortName(gufo::tokenization::QwenReasoningEffort effort) {
  using E = gufo::tokenization::QwenReasoningEffort;
  switch (effort) {
    case E::kLow: return "low";
    case E::kMedium: return "medium";
    case E::kXHigh: return "xhigh";
  }
  return "xhigh";
}

gufo::tokenization::ChatRole ParseRole(std::string_view role) {
  using R = gufo::tokenization::ChatRole;
  if (role == "system") return R::kSystem;
  if (role == "developer") return R::kDeveloper;
  if (role == "assistant") return R::kAssistant;
  if (role == "tool") return R::kTool;
  return R::kUser;
}

bool ParseContent(const Value* content, gufo::tokenization::ChatMessage* message,
                  gufo::core::ImageReadBudget* image_budget, std::string* error) {
  if (!content || content->is_null()) return true;
  if (content->is_string()) {
    message->content = content->get_str();
    return true;
  }
  if (!content->is_array()) {
    *error = "message content must be string/null/array";
    return false;
  }
  for (const auto& part : content->items()) {
    if (!part.is_object()) { *error = "content part must be object"; return false; }
    const auto type = part.member_str("type", "text");
    if (type == "text" || type == "input_text") {
      const auto* text = part.find("text");
      if (!text || !text->is_string()) { *error = "text content requires text"; return false; }
      message->content += text->get_str();
      continue;
    }
    if (type == "image_url") {
      const auto* obj = part.find("image_url");
      const auto* url = obj && obj->is_object() ? obj->find("url") : nullptr;
      if (!url || !url->is_string() || message->role != gufo::tokenization::ChatRole::kUser) {
        *error = "image_url requires a user message and URL";
        return false;
      }
      try {
        auto bytes = gufo::core::ReadImageUrl(url->get_str(), *image_budget);
        message->images.push_back({message->content.size(),
            std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes))});
      } catch (const std::exception& e) {
        *error = e.what();
        return false;
      }
      continue;
    }
    *error = "unsupported content part type: " + type;
    return false;
  }
  return true;
}

ChatInput ParseChat(const Value& body, const Config& cfg) {
  ChatInput out;
  out.enable_thinking = cfg.thinking;
  out.preserve_thinking = cfg.preserve_thinking;
  out.reasoning_effort = ParseEffort(cfg.reasoning_effort);

  if (const auto* kwargs = body.find("chat_template_kwargs"); kwargs && kwargs->is_object()) {
    if (const auto* v = kwargs->find("enable_thinking"); v && v->is_bool()) out.enable_thinking = v->as_bool();
    if (const auto* v = kwargs->find("preserve_thinking"); v && v->is_bool()) out.preserve_thinking = v->as_bool();
    if (const auto* v = kwargs->find("reasoning_effort"); v && v->is_string()) out.reasoning_effort = ParseEffort(v->get_str());
    if (const auto* v = kwargs->find("add_vision_id"); v && v->is_bool()) out.add_vision_id = v->as_bool();
  }
  if (const auto* v = body.find("reasoning_effort"); v && v->is_string()) {
    if (v->get_str() == "off" || v->get_str() == "none") out.enable_thinking = false;
    else out.reasoning_effort = ParseEffort(v->get_str());
  }

  const auto* messages = body.find("messages");
  if (!messages || !messages->is_array() || messages->empty())
    throw std::invalid_argument("messages must be a non-empty array");
  gufo::core::ImageReadBudget budget;
  for (const auto& item : messages->items()) {
    if (!item.is_object()) throw std::invalid_argument("message must be an object");
    const auto role = item.member_str("role");
    if (role != "system" && role != "developer" && role != "user" && role != "assistant" && role != "tool")
      throw std::invalid_argument("invalid message role");
    gufo::tokenization::ChatMessage msg;
    msg.role = ParseRole(role);
    msg.name = item.member_str("name");
    msg.tool_call_id = item.member_str("tool_call_id");
    std::string error;
    if (!ParseContent(item.find("content"), &msg, &budget, &error))
      throw std::invalid_argument(error);
    if (const auto* r = item.find("reasoning_content"); r && r->is_string()) msg.thought = r->get_str();
    if (const auto* calls = item.find("tool_calls"); calls && calls->is_array()) {
      for (const auto& callv : calls->items()) {
        const auto* fn = callv.find("function");
        if (!fn || !fn->is_object()) continue;
        gufo::tokenization::ChatMessage::ToolCall call;
        call.id = callv.member_str("id");
        call.name = fn->member_str("name");
        const auto args_text = fn->member_str("arguments", "{}");
        Value args = gufo::json::parse(args_text);
        if (args.is_object()) {
          for (const auto& [name, val] : args.members()) {
            call.arguments.push_back({name, val.is_string() ? val.get_str() : val.dump(), val.is_string()});
          }
        }
        msg.tool_calls.push_back(std::move(call));
      }
    }
    out.messages.push_back(std::move(msg));
  }

  if (const auto* tools = body.find("tools"); tools && tools->is_array()) {
    for (const auto& item : tools->items()) {
      if (!item.is_object() || item.member_str("type") != "function") continue;
      const auto* fn = item.find("function");
      if (!fn || !fn->is_object()) continue;
      gufo::tokenization::ChatTool t;
      t.name = fn->member_str("name");
      t.description = fn->member_str("description");
      if (const auto* p = fn->find("parameters")) t.parameters_json = p->dump();
      t.definition_json = item.dump();
      if (!t.name.empty()) out.tools.push_back(std::move(t));
    }
  }
  if (const auto* choice = body.find("tool_choice"); choice && choice->is_string()) {
    out.tool_required = choice->get_str() == "required";
    out.tool_none = choice->get_str() == "none";
  }
  if (out.tool_none) out.tools.clear();
  return out;
}

gufo::sampling::SamplingConfig ParseSampling(const Value& body, const Config& cfg) {
  auto s = cfg.sampling;
  if (const auto* v = body.find("temperature"); v && v->is_number()) s.temperature = static_cast<float>(v->as_double());
  if (const auto* v = body.find("top_p"); v && v->is_number()) s.top_p = static_cast<float>(v->as_double());
  if (const auto* v = body.find("top_k"); v && v->is_number()) s.top_k = static_cast<std::int32_t>(v->as_double());
  if (const auto* v = body.find("min_p"); v && v->is_number()) s.min_p = static_cast<float>(v->as_double());
  if (const auto* v = body.find("seed"); v && v->is_number()) s.seed = static_cast<std::int64_t>(v->as_double());
  if (const auto* v = body.find("repeat_penalty"); v && v->is_number()) s.repeat_penalty = static_cast<float>(v->as_double());
  if (const auto* v = body.find("repeat_last_n"); v && v->is_number()) s.repeat_last_n = v->as_size(s.repeat_last_n);
  if (const auto* v = body.find("frequency_penalty"); v && v->is_number()) s.frequency_penalty = static_cast<float>(v->as_double());
  if (const auto* v = body.find("presence_penalty"); v && v->is_number()) s.presence_penalty = static_cast<float>(v->as_double());
  s.Validate();
  return s;
}

std::size_t MaxTokens(const Value& body, const Config& cfg) {
  if (const auto* v = body.find("max_completion_tokens"); v && v->is_number()) return v->as_size(cfg.default_max_tokens);
  if (const auto* v = body.find("max_tokens"); v && v->is_number()) return v->as_size(cfg.default_max_tokens);
  return cfg.default_max_tokens;
}

std::vector<std::int32_t> ToI32(std::span<const gufo::tokenization::TokenId> src) {
  std::vector<std::int32_t> out;
  out.reserve(src.size());
  for (auto x : src) out.push_back(static_cast<std::int32_t>(x));
  return out;
}

std::vector<gufo::sampling::TokenId> ToSampler(std::span<const std::int32_t> src) {
  std::vector<gufo::sampling::TokenId> out;
  out.reserve(src.size());
  for (auto x : src) out.push_back(static_cast<gufo::sampling::TokenId>(x));
  return out;
}

std::optional<Value> TryJson(std::string_view text) {
  try { return gufo::json::parse(text); } catch (...) { return std::nullopt; }
}

std::vector<ToolCall> ParseToolCalls(std::string_view text) {
  std::vector<ToolCall> calls;
  constexpr std::string_view open = "<tool_call>";
  constexpr std::string_view close = "</tool_call>";
  std::size_t pos = 0;
  while ((pos = text.find(open, pos)) != std::string_view::npos) {
    const std::size_t begin = pos + open.size();
    const std::size_t end = text.find(close, begin);
    if (end == std::string_view::npos) break;
    std::string body = Trim(text.substr(begin, end - begin));
    ToolCall call;
    call.id = RandomId("call_");
    if (auto parsed = TryJson(body); parsed && parsed->is_object()) {
      call.name = parsed->member_str("name");
      if (const auto* args = parsed->find("arguments"); args && args->is_object()) call.arguments = *args;
    } else if (body.starts_with("<function=")) {
      const auto nend = body.find('>');
      if (nend != std::string::npos) {
        call.name = Trim(std::string_view(body).substr(10, nend - 10));
        Value args = Value::object();
        std::size_t p = nend + 1;
        while ((p = body.find("<parameter=", p)) != std::string::npos) {
          const auto pe = body.find('>', p);
          const auto ce = body.find("</parameter>", pe);
          if (pe == std::string::npos || ce == std::string::npos) break;
          const std::string name = Trim(std::string_view(body).substr(p + 11, pe - (p + 11)));
          const std::string raw = Trim(std::string_view(body).substr(pe + 1, ce - pe - 1));
          if (auto v = TryJson(raw)) args[name] = *v; else args[name] = raw;
          p = ce + 12;
        }
        call.arguments = std::move(args);
      }
    }
    if (!call.name.empty()) calls.push_back(std::move(call));
    pos = end + close.size();
  }
  return calls;
}

ParsedOutput ParseOutput(std::string raw, bool thinking, bool tools_enabled) {
  ParsedOutput out;
  std::string working = std::move(raw);
  if (thinking) {
    if (working.rfind("<think>", 0) == 0) working.erase(0, 7);
    const auto end = working.find("</think>");
    if (end != std::string::npos) {
      out.reasoning = Trim(std::string_view(working).substr(0, end));
      working.erase(0, end + 8);
      while (!working.empty() && (working.front() == '\n' || working.front() == '\r')) working.erase(working.begin());
    }
  } else {
    const auto begin = working.find("<think>");
    const auto end = working.find("</think>");
    if (begin != std::string::npos && end != std::string::npos && end > begin) {
      out.reasoning = Trim(std::string_view(working).substr(begin + 7, end - begin - 7));
      working.erase(begin, end + 8 - begin);
    }
  }
  if (tools_enabled) {
    out.calls = ParseToolCalls(working);
    if (!out.calls.empty()) {
      const auto marker = working.find("<tool_call>");
      if (marker != std::string::npos) working.resize(marker);
    }
  }
  out.content = Trim(working);
  return out;
}

std::string FormatFixed(double value, int digits) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(digits) << value;
  return out.str();
}

std::string FormatEffectiveSettings(const Metrics& m) {
  std::ostringstream o;
  o << "EFFECTIVE SETTINGS\n";
  o << "Thinking: " << (m.thinking ? "ON" : "OFF") << "\n";
  o << "Preserve thinking: " << (m.preserve_thinking ? "ON" : "OFF") << "\n";
  o << "Reasoning effort: " << (m.thinking ? m.reasoning_effort : "OFF") << "\n";
  o << "Temperature: " << FormatFixed(m.temperature, 2) << "\n";
  o << "Top P: " << FormatFixed(m.top_p, 2) << "\n";
  o << "Top K: " << m.top_k << "\n";
  o << "Min P: " << FormatFixed(m.min_p, 2) << "\n";
  o << "Repeat penalty: " << FormatFixed(m.repeat_penalty, 2) << "\n";
  o << "Repeat last N: " << m.repeat_last_n << "\n";
  o << "MTP draft max: " << m.draft_max << "\n";
  o << "MTP draft confidence: " << FormatFixed(m.draft_confidence, 2) << "\n";
  o << "MTP proposal mode: " << MtpProposalModeDisplay(m.mtp_proposal_mode) << "\n";
  o << "Prefill batch: " << m.prefill_batch << " (real Gufo max_batch)\n";
  o << "Context lookup: " << (m.context_lookup_enabled ? "ON (native exact-verification lane)" : "OFF") << "\n";
  o << "MTP cost profile: Gufo calibrated\n";
  o << "MTP hidden normalization: per-HC-stream\n";
  o << "MTP async halo chain: "
    << ((m.mtp_proposal_mode == "halo_greedy" && !(m.draft_confidence > 0.0))
            ? "ON (GPU feedback; one chain-end sync)"
            : "OFF (requires halo-greedy + confidence 0.00)")
    << "\n";
  o << "Verification frontier: GPU-retained + lazy full-row D2H\n";
  o << "MTP draft candidate support: 256\n";
  o << "MTP target verification shortlist: ";
  if (m.compact_verification_candidates != 0)
    o << "Top-" << m.compact_verification_candidates << " exact\n";
  else
    o << "full-vocabulary fallback\n";
  o << "Context: " << m.context_capacity << "\n\n";
  return o.str();
}

std::string FormatProfile(const Metrics& m) {
  const auto& p = m.profile;
  if (!p.present) return {};
  const double inter = m.completion_tokens ? m.decode_ms / static_cast<double>(m.completion_tokens) : 0;
  const double rejected = static_cast<double>(m.draft_tokens - m.draft_accepted);
  const double per_out = m.completion_tokens
                             ? static_cast<double>(m.draft_accepted) / static_cast<double>(m.completion_tokens)
                             : 0;
  std::ostringstream o;
  o << std::fixed;
  o << "GENERAL\n";
  o << "Prompt tokens                 " << m.prompt_tokens << "\n";
  o << "Completion tokens             " << m.completion_tokens << "\n";
  o << "Prefill ms                    " << FormatFixed(m.prefill_ms, 1) << "\n";
  o << "Prefill tok/s                 " << FormatFixed(m.prefill_tps, 2) << "\n";
  o << "Decode ms                     " << FormatFixed(m.decode_ms, 1) << "\n";
  o << "Decode tok/s                  " << FormatFixed(m.decode_tps, 2) << "\n";
  o << "TTFT ms                       " << FormatFixed(p.ttft_ms, 1) << "\n";
  o << "Avg inter-token ms            " << FormatFixed(inter, 2) << "\n";
  o << "Context capacity              " << p.context_capacity << "\n";
  o << "Context depth at start        " << p.context_depth << "\n\n";
  o << FormatEffectiveSettings(m);
  o << "SPECULATION\n";
  o << "Draft max                     " << p.draft_max << "\n";
  o << "Avg chosen draft depth        " << FormatFixed(p.avg_draft_depth, 2) << "\n";
  o << "MTP-only avg draft depth      " << FormatFixed(p.mtp_avg_draft_depth, 2) << "\n";
  o << "Lookup avg draft depth        " << FormatFixed(p.lookup_avg_draft_depth, 2) << "\n";
  o << "Drafted tokens                " << m.draft_tokens << "\n";
  o << "Accepted tokens               " << m.draft_accepted << "\n";
  o << "Rejected tokens               " << static_cast<unsigned long long>(rejected) << "\n";
  o << "Acceptance                    " << FormatFixed(m.draft_acceptance * 100.0, 1) << "%\n";
  o << "Accepted / output token       " << FormatFixed(per_out, 3) << "\n";
  const double mtp_acceptance = m.mtp_drafted ? static_cast<double>(m.mtp_accepted) / static_cast<double>(m.mtp_drafted) : 0.0;
  const double lookup_acceptance = m.lookup_drafted ? static_cast<double>(m.lookup_accepted) / static_cast<double>(m.lookup_drafted) : 0.0;
  o << "MTP-only drafted              " << m.mtp_drafted << "\n";
  o << "MTP-only accepted             " << m.mtp_accepted << "\n";
  o << "MTP-only acceptance           " << FormatFixed(mtp_acceptance * 100.0, 1) << "%\n";
  o << "MTP avg proposal confidence   " << FormatFixed(m.mtp_avg_proposal_confidence, 3) << "\n";
  o << "Lookup cycles                 " << m.lookup_cycles << "\n";
  o << "Lookup drafted                " << m.lookup_drafted << "\n";
  o << "Lookup accepted               " << m.lookup_accepted << "\n";
  o << "Lookup acceptance             " << FormatFixed(lookup_acceptance * 100.0, 1) << "%\n";
  o << "Lookup avg n-gram             " << FormatFixed(m.lookup_avg_ngram, 2) << "\n";
  o << "Lookup avg source distance    " << FormatFixed(m.lookup_avg_distance, 1) << " tokens\n";
  o << "MTP draft confidence           " << FormatFixed(m.draft_confidence, 2) << "\n";
  o << "MTP proposal mode              " << MtpProposalModeDisplay(m.mtp_proposal_mode) << "\n";
  o << "Prefill batch                  " << m.prefill_batch << "\n";
  o << "MTP cost profile              Gufo calibrated\n";
  o << "MTP hidden normalization      per-HC-stream\n";
  o << "MTP draft candidate support   256\n";
  o << "MTP target verify shortlist   ";
  if (m.compact_verification_candidates != 0)
    o << "Top-" << m.compact_verification_candidates << " exact\n";
  else
    o << "full-vocabulary fallback\n";
  o << "Low-confidence draft stops     " << m.low_confidence_stops << "\n";
  o << "Compact frontier samples       " << m.compact_frontier_samples << "\n";
  o << "Frontier certificate fallbacks " << m.compact_frontier_fallbacks << "\n";
  o << "Retained frontier downloads    " << m.retained_frontier_downloads << "\n";
  o << "Async halo chains              " << m.async_halo_chains << "\n";
  o << "Async halo draft tokens        " << m.async_halo_draft_tokens << "\n";
  o << "MTP fresh-state resets         " << m.mtp_fresh_resets << "\n";
  o << "Draft confidence applies to sampled MTP. Greedy drafts are unchanged.\n";
  o << "All MTP-state catch-up ms     " << FormatFixed(p.catchup_ms, 1) << "\n";
  o << "MTP prep/proposal ms          " << FormatFixed(p.mtp_proposal_ms, 1) << "\n";
  o << "MTP target verification ms    " << FormatFixed(p.mtp_verification_ms, 1) << "\n";
  o << "MTP speculative cycle wall ms " << FormatFixed(p.mtp_total_cycle_ms, 1) << "\n";
  o << "Lookup prep/state ms          " << FormatFixed(p.lookup_proposal_ms, 1) << "\n";
  o << "Lookup target verify ms       " << FormatFixed(p.lookup_verification_ms, 1) << "\n";
  o << "Lookup speculative wall ms    " << FormatFixed(p.lookup_total_cycle_ms, 1) << "\n";
  o << "All target verification ms    " << FormatFixed(p.verification_ms, 1) << "\n";
  o << "All rollback ms               " << FormatFixed(p.rollback_ms, 1) << "\n";
  o << "HIP graph captures            " << p.graph_captures << "\n";
  const std::uint64_t rounds = p.mtp_cycles + p.lookup_cycles;
  o << "Speculative rounds            " << rounds << "\n";
  o << "MTP rounds                    " << p.mtp_cycles << "\n";
  o << "MTP width proposed / round    "
    << FormatFixed(p.mtp_cycles ? static_cast<double>(p.mtp_drafted) / p.mtp_cycles : 0.0, 2) << "\n";
  o << "MTP yield emitted / round     "
    << FormatFixed(p.mtp_cycles ? static_cast<double>(p.mtp_output_tokens) / p.mtp_cycles : 0.0, 2) << "\n";
  o << "MTP verify ms / round         "
    << FormatFixed(p.mtp_cycles ? p.mtp_verification_ms / p.mtp_cycles : 0.0, 2) << "\n";
  o << "MTP verify ms / output        "
    << FormatFixed(p.mtp_output_tokens ? p.mtp_verification_ms / p.mtp_output_tokens : 0.0, 3) << "\n";
  o << "Lookup rounds                 " << p.lookup_cycles << "\n";
  o << "Lookup width / round          "
    << FormatFixed(p.lookup_cycles ? static_cast<double>(p.lookup_drafted) / p.lookup_cycles : 0.0, 2) << "\n";
  o << "Lookup yield / round          "
    << FormatFixed(p.lookup_cycles ? static_cast<double>(p.lookup_output_tokens) / p.lookup_cycles : 0.0, 2) << "\n";
  o << "Lookup verify ms / round      "
    << FormatFixed(p.lookup_cycles ? p.lookup_verification_ms / p.lookup_cycles : 0.0, 2) << "\n";
  o << "Lookup verify ms / output     "
    << FormatFixed(p.lookup_output_tokens ? p.lookup_verification_ms / p.lookup_output_tokens : 0.0, 3) << "\n";
  const auto write_survival = [&](std::string_view label, const std::uint64_t* proposed,
                                  const std::uint64_t* accepted) {
    o << label;
    bool any = false;
    for (int i = 0; i < fnvprof::kDepths - 1; ++i) {
      if (proposed[i] == 0) break;
      if (any) o << ", ";
      any = true;
      o << (i + 1) << ":"
        << FormatFixed(100.0 * static_cast<double>(accepted[i]) /
                           static_cast<double>(proposed[i]),
                       1)
        << "%";
    }
    if (!any) o << "n/a";
    o << "\n";
  };
  write_survival("MTP prefix survival           ", p.mtp_proposed_per_position,
                 p.mtp_accepted_per_position);
  write_survival("Lookup prefix survival        ", p.lookup_proposed_per_position,
                 p.lookup_accepted_per_position);
  o << "\n";
  o << "Verification substages\n";
  o << "VERIFY TRUNK ms               " << FormatFixed(p.verify_trunk_ms, 1) << "\n";
  o << "HC HEAD ms                    " << FormatFixed(p.verify_hc_head_ms, 1) << "\n";
  o << "VOCAB PROJECTION ms           " << FormatFixed(p.verify_vocab_projection_ms, 1) << "\n";
  o << "COMPACT SELECT ms             " << FormatFixed(p.verify_compact_select_ms, 1) << "\n";
  o << "D2H ms                        " << FormatFixed(p.verify_d2h_ms, 1) << "\n";
  o << "CPU ACCEPT / RESIDUAL ms      " << FormatFixed(p.verify_cpu_accept_residual_ms, 1) << "\n";
  o << "Verification GPU events       "
    << (p.verify_gpu_events_reliable ? "recorded" : "incomplete") << "\n";
  o << "Verify output rows            " << p.verify_output_rows << "\n";
  o << "Full rows D2H                 " << p.verify_full_rows_d2h << "\n";
  o << "Compact rows D2H              " << p.verify_compact_rows_d2h << "\n";
  o << "Verification D2H bytes        " << p.verify_d2h_bytes << "\n";
  o << "Compact candidates transferred " << p.verify_compact_candidates << "\n";
  o << "Certificate successes         " << p.verify_certificate_successes << "\n";
  o << "Certificate fallbacks         " << p.verify_certificate_fallbacks << "\n";
  o << "Unsupported sampler fallbacks " << p.verify_unsupported_sampler_fallbacks << "\n";
  o << "Rejection frontier downloads  " << p.verify_rejection_frontier_downloads << "\n";
  o << "GPU substage event timings are eager-only; graph replay is marked incomplete. "
       "Transfer/certificate counters remain complete.\n\n";
  const auto write_depth_table = [&](std::string_view title, const fnvprof::DepthRow* rows) {
    o << title << "\n";
    o << "depth  cycles      avg ms    accept    out tok/s  proposal/round  verify/round  verify/out\n";
    for (int depth = 0; depth < fnvprof::kDepths; ++depth) {
      const auto& row = rows[depth];
      const double avg = row.cycles ? row.cycle_ms / static_cast<double>(row.cycles) : 0;
      const double accept = row.drafted ? 100.0 * static_cast<double>(row.accepted) / static_cast<double>(row.drafted) : 0;
      const double tps = row.cycle_ms > 0 ? 1000.0 * static_cast<double>(row.output_tokens) / row.cycle_ms : 0;
      const double proposal_round = row.cycles ? row.proposal_ms / static_cast<double>(row.cycles) : 0;
      const double verify_round = row.cycles ? row.verify_ms / static_cast<double>(row.cycles) : 0;
      const double verify_out = row.output_tokens ? row.verify_ms / static_cast<double>(row.output_tokens) : 0;
      o << std::setw(5) << depth << std::setw(8) << row.cycles << std::setw(12) << FormatFixed(avg, 2)
        << std::setw(10) << (row.drafted ? FormatFixed(accept, 1) + "%" : std::string("n/a"))
        << std::setw(12) << FormatFixed(tps, 2)
        << std::setw(16) << FormatFixed(proposal_round, 2)
        << std::setw(14) << FormatFixed(verify_round, 2)
        << std::setw(12) << FormatFixed(verify_out, 3) << "\n";
    }
  };
  write_depth_table("MTP draft depth", p.mtp_depths);
  o << "\n";
  write_depth_table("Lookup draft depth", p.lookup_depths);
  o << "\n64-token windows\n";
  o << "tokens        tok/s   accept   depth   draft ms  verify ms    QSA ms    PLE ms\n";
  for (const auto& w : p.windows) {
    o << std::setw(5) << w.token_lo << "-" << std::setw(5) << w.token_hi
      << std::setw(8) << FormatFixed(w.tokens_per_second, 2)
      << std::setw(8) << FormatFixed(w.acceptance * 100.0, 1) << "%"
      << std::setw(8) << FormatFixed(w.avg_draft_depth, 2)
      << std::setw(11) << FormatFixed(w.proposal_ms, 1)
      << std::setw(11) << FormatFixed(w.verify_ms, 1)
      << std::setw(10) << FormatFixed(w.qsa_ms, 1)
      << std::setw(10) << FormatFixed(w.ple_wait_ms, 1) << "\n";
  }
  const double qsa_per = m.completion_tokens ? p.qsa_total_ms / static_cast<double>(m.completion_tokens) : 0;
  const double qsa_pct = m.decode_ms > 0 ? 100.0 * p.qsa_total_ms / m.decode_ms : 0;
  o << "\nQSA\n";
  o << "QSA/indexer ms                " << FormatFixed(p.qsa_indexer_ms, 1) << "\n";
  o << "Selector/ranking ms           " << FormatFixed(p.qsa_selector_ms, 1) << "\n";
  o << "Sparse attention ms           " << FormatFixed(p.qsa_sparse_ms, 1) << "\n";
  o << "Total QSA ms                  " << FormatFixed(p.qsa_total_ms, 1) << "\n";
  o << "QSA ms/output token           " << FormatFixed(qsa_per, 3) << "\n";
  o << "QSA % of decode               " << FormatFixed(qsa_pct, 1) << "%\n";
  o << "QSA events                    " << (p.qsa_events_reliable ? "recorded" : "incomplete") << "\n";
  const double ple_per = m.completion_tokens ? p.ple_wait_ms / static_cast<double>(m.completion_tokens) : 0;
  o << "\nPLE / ngram\n";
  o << "Disk row reads                " << p.ple_reads << "\n";
  o << "Disk-read ms (worker sum)      " << FormatFixed(p.ple_disk_ms, 1) << "\n";
  o << "WaitRead blocking ms          " << FormatFixed(p.ple_wait_ms, 1) << "\n";
  o << "Avg WaitRead ms               " << FormatFixed(p.ple_avg_wait_ms, 3) << "\n";
  o << "Max WaitRead ms               " << FormatFixed(p.ple_max_wait_ms, 3) << "\n";
  o << "PLE wait ms/output token      " << FormatFixed(ple_per, 3) << "\n";
  o << "\nhipBLASLt / hipBLAS (prefill + decode)\n";
  o << "HIPBLASLT_TENSILE_LIBPATH     " << (p.tensile_libpath.empty() ? "(not set)" : p.tensile_libpath) << "\n";
  o << "hipBLASLt calls               " << p.lt_calls << "\n";
  o << "Successful hipBLASLt calls    " << p.lt_success << "\n";
  o << "hipBLAS fallback calls        " << p.hipblas_fallback << "\n";
  o << "Direct hipBLAS calls          " << p.hipblas_direct << "\n";
  o << "Fallback %                    " << FormatFixed(p.fallback_pct, 1) << "%\n";
  if (!p.fallback_shapes.empty()) {
    o << "Fallback / direct shapes\n";
    o << "kind      m      n      k   type   count\n";
    for (const auto& shape : p.fallback_shapes) {
      o << std::setw(8) << (shape.kind == 0 ? "fallback" : "direct")
        << std::setw(7) << shape.m << std::setw(7) << shape.n << std::setw(7) << shape.k
        << std::setw(7) << shape.type << std::setw(8) << shape.count << "\n";
    }
  }
  const double sync_per = m.completion_tokens ? p.sync_ms / static_cast<double>(m.completion_tokens) : 0;
  o << "\nGPU/CPU synchronization (decode)\n";
  o << "hipStreamSynchronize          " << p.stream_syncs << "\n";
  o << "hipDeviceSynchronize          " << p.device_syncs << "\n";
  if (p.event_syncs) o << "hipEventSynchronize           " << p.event_syncs << "\n";
  o << "Synchronization wait ms       " << FormatFixed(p.sync_ms, 1) << "\n";
  o << "Sync ms/output token          " << FormatFixed(sync_per, 3) << "\n";
  o << "\nDecode stage                         ms         %\n";
  for (const auto& stage : p.stages) {
    o << std::left << std::setw(32) << stage.name << std::right
      << std::setw(12) << FormatFixed(stage.ms, 1)
      << std::setw(10) << FormatFixed(stage.pct, 1) << "%\n";
  }
  o << "\nStage rows are exclusive: QSA, PLE wait, and synchronization are removed from the stage that contained them.\n";
  o << "Raw MTP, QSA, PLE, and sync totals above are the measured costs before that split.\n";
  if (p.qsa_capture_skipped) {
    o << "QSA GPU events are recorded on eager launches only. HIP graph replay is not included, so window QSA does not show whether QSA grows across the run.\n";
  } else if (!p.qsa_events_reliable) {
    o << "QSA GPU events did not cover every decode step. Treat the QSA milliseconds as incomplete.\n";
  }
  if (p.graph_captures) {
    o << "The first profiled use of a decode shape records a private HIP graph. That one-time cost is inside the early cycles.\n";
  }
  return o.str();
}

std::string FormatLastRequest(const Metrics& m) {
  if (!m.has_request) return "No request yet.\n";
  const double rejected = static_cast<double>(m.draft_tokens - m.draft_accepted);
  const double per_out = m.completion_tokens
                             ? static_cast<double>(m.draft_accepted) / static_cast<double>(m.completion_tokens)
                             : 0;
  std::ostringstream o;
  o << std::fixed;
  o << "LAST REQUEST\n";
  o << "GENERAL\n";
  o << "Prompt tokens                 " << m.prompt_tokens << "\n";
  o << "Completion tokens             " << m.completion_tokens << "\n";
  o << "Prefill ms                    " << FormatFixed(m.prefill_ms, 1) << "\n";
  o << "Prefill tok/s                 " << FormatFixed(m.prefill_tps, 2) << "\n";
  o << "Decode ms                     " << FormatFixed(m.decode_ms, 1) << "\n";
  o << "Decode tok/s                  " << FormatFixed(m.decode_tps, 2) << "\n";
  o << "TTFT ms                       " << FormatFixed(m.ttft_ms, 1) << "\n";
  o << "Avg inter-token ms            " << FormatFixed(m.avg_inter_token_ms, 2) << "\n";
  o << "Context capacity              " << m.context_capacity << "\n";
  o << "Context depth at start        " << m.context_depth << "\n\n";
  o << FormatEffectiveSettings(m);
  o << "SPECULATION\n";
  o << "Draft max                     " << m.draft_max << "\n";
  o << "Avg chosen draft depth        " << FormatFixed(m.avg_draft_depth, 2) << "\n";
  o << "All speculative drafted       " << m.draft_tokens << "\n";
  o << "All speculative accepted      " << m.draft_accepted << "\n";
  o << "All speculative rejected      " << static_cast<unsigned long long>(rejected) << "\n";
  o << "All speculative acceptance    " << FormatFixed(m.draft_acceptance * 100.0, 1) << "%\n";
  o << "Accepted / output token       " << FormatFixed(per_out, 3) << "\n";
  const double mtp_acceptance = m.mtp_drafted ? static_cast<double>(m.mtp_accepted) / static_cast<double>(m.mtp_drafted) : 0.0;
  const double lookup_acceptance = m.lookup_drafted ? static_cast<double>(m.lookup_accepted) / static_cast<double>(m.lookup_drafted) : 0.0;
  o << "MTP-only drafted              " << m.mtp_drafted << "\n";
  o << "MTP-only accepted             " << m.mtp_accepted << "\n";
  o << "MTP-only acceptance           " << FormatFixed(mtp_acceptance * 100.0, 1) << "%\n";
  o << "MTP avg proposal confidence   " << FormatFixed(m.mtp_avg_proposal_confidence, 3) << "\n";
  o << "Lookup cycles                 " << m.lookup_cycles << "\n";
  o << "Lookup drafted                " << m.lookup_drafted << "\n";
  o << "Lookup accepted               " << m.lookup_accepted << "\n";
  o << "Lookup acceptance             " << FormatFixed(lookup_acceptance * 100.0, 1) << "%\n";
  o << "Lookup avg n-gram             " << FormatFixed(m.lookup_avg_ngram, 2) << "\n";
  o << "Lookup avg source distance    " << FormatFixed(m.lookup_avg_distance, 1) << " tokens\n";
  o << "MTP draft confidence           " << FormatFixed(m.draft_confidence, 2) << "\n";
  o << "MTP proposal mode              " << MtpProposalModeDisplay(m.mtp_proposal_mode) << "\n";
  o << "Prefill batch                  " << m.prefill_batch << "\n";
  o << "Low-confidence draft stops     " << m.low_confidence_stops << "\n";
  o << "Compact frontier samples       " << m.compact_frontier_samples << "\n";
  o << "Frontier certificate fallbacks " << m.compact_frontier_fallbacks << "\n";
  o << "Retained frontier downloads    " << m.retained_frontier_downloads << "\n";
  o << "Async halo chains              " << m.async_halo_chains << "\n";
  o << "Async halo draft tokens        " << m.async_halo_draft_tokens << "\n";
  o << "MTP fresh-state resets         " << m.mtp_fresh_resets << "\n";
  o << "Draft confidence applies to sampled MTP. Greedy drafts are unchanged.\n\n";
  o << "64-token windows\n";
  if (m.request_windows.empty()) {
    o << "Not enough tokens for a window.\n";
  } else {
    o << "tokens        tok/s   accept   depth\n";
    for (const auto& w : m.request_windows) {
      o << std::setw(5) << w.token_lo << "-" << std::setw(5) << w.token_hi
        << std::setw(8) << FormatFixed(w.tokens_per_second, 2)
        << std::setw(8) << FormatFixed(w.acceptance * 100.0, 1) << "%"
        << std::setw(8) << FormatFixed(w.avg_draft_depth, 2) << "\n";
    }
  }
  return o.str();
}

Value MetricsJson(const Metrics& m) {
  Value v = Value::object();
  v["prompt_tokens"] = static_cast<unsigned long long>(m.prompt_tokens);
  v["completion_tokens"] = static_cast<unsigned long long>(m.completion_tokens);
  v["prefill_ms"] = m.prefill_ms;
  v["decode_ms"] = m.decode_ms;
  v["prefill_tokens_per_second"] = m.prefill_tps;
  v["completion_tokens_per_second"] = m.decode_tps;
  v["ttft_ms"] = m.ttft_ms;
  v["avg_inter_token_ms"] = m.avg_inter_token_ms;
  v["context_capacity"] = static_cast<unsigned long long>(m.context_capacity);
  v["context_depth"] = static_cast<unsigned long long>(m.context_depth);
  v["draft_max"] = static_cast<unsigned long long>(m.draft_max);
  v["avg_draft_depth"] = m.avg_draft_depth;
  v["draft_confidence"] = m.draft_confidence;
  v["mtp_proposal_mode"] = m.mtp_proposal_mode;
  v["prefill_batch"] = static_cast<unsigned long long>(m.prefill_batch);
  v["compact_verification_candidates"] =
      static_cast<unsigned long long>(m.compact_verification_candidates);
  v["low_confidence_stops"] = static_cast<unsigned long long>(m.low_confidence_stops);
  v["compact_frontier_samples"] = static_cast<unsigned long long>(m.compact_frontier_samples);
  v["compact_frontier_fallbacks"] = static_cast<unsigned long long>(m.compact_frontier_fallbacks);
  v["retained_frontier_downloads"] = static_cast<unsigned long long>(m.retained_frontier_downloads);
  v["async_halo_chains"] = static_cast<unsigned long long>(m.async_halo_chains);
  v["async_halo_draft_tokens"] = static_cast<unsigned long long>(m.async_halo_draft_tokens);
  v["mtp_fresh_resets"] = static_cast<unsigned long long>(m.mtp_fresh_resets);
  v["mtp_draft_tokens"] = static_cast<unsigned long long>(m.mtp_drafted);
  v["mtp_draft_tokens_accepted"] = static_cast<unsigned long long>(m.mtp_accepted);
  v["mtp_acceptance"] = m.mtp_drafted ? static_cast<double>(m.mtp_accepted) / static_cast<double>(m.mtp_drafted) : 0.0;
  v["mtp_avg_proposal_confidence"] = m.mtp_avg_proposal_confidence;
  v["lookup_cycles"] = static_cast<unsigned long long>(m.lookup_cycles);
  v["lookup_draft_tokens"] = static_cast<unsigned long long>(m.lookup_drafted);
  v["lookup_draft_tokens_accepted"] = static_cast<unsigned long long>(m.lookup_accepted);
  v["lookup_acceptance"] = m.lookup_drafted ? static_cast<double>(m.lookup_accepted) / static_cast<double>(m.lookup_drafted) : 0.0;
  v["lookup_avg_ngram"] = m.lookup_avg_ngram;
  v["lookup_avg_source_distance"] = m.lookup_avg_distance;
  v["context_lookup_active"] = m.context_lookup_enabled;
  v["memory_available_bytes"] = static_cast<unsigned long long>(m.memory_available_bytes);
  v["memory_commit_available_bytes"] = static_cast<unsigned long long>(m.memory_commit_available_bytes);
  v["draft_tokens"] = static_cast<unsigned long long>(m.draft_tokens);
  v["draft_tokens_accepted"] = static_cast<unsigned long long>(m.draft_accepted);
  v["draft_tokens_rejected"] = static_cast<unsigned long long>(m.draft_tokens - m.draft_accepted);
  v["draft_acceptance"] = m.draft_acceptance;
  v["accepted_per_output_token"] = m.completion_tokens
                                      ? static_cast<double>(m.draft_accepted) / static_cast<double>(m.completion_tokens)
                                      : 0;
  Value effective = Value::object();
  effective["thinking"] = m.thinking;
  effective["preserve_thinking"] = m.preserve_thinking;
  effective["reasoning_effort"] = m.thinking ? m.reasoning_effort : "OFF";
  effective["mtp_policy_mode"] = "gufo_calibrated";
  effective["mtp_hidden_normalization"] = "per_hc_stream";
  effective["mtp_async_halo_chain"] =
      m.mtp_proposal_mode == "halo_greedy" && !(m.draft_confidence > 0.0);
  effective["verification_frontier_transfer"] = "gpu_retained_lazy_d2h";
  effective["compact_verification_candidates"] =
      static_cast<unsigned long long>(m.compact_verification_candidates);
  effective["temperature"] = m.temperature;
  effective["top_p"] = m.top_p;
  effective["top_k"] = m.top_k;
  effective["min_p"] = m.min_p;
  effective["repeat_penalty"] = m.repeat_penalty;
  effective["repeat_last_n"] = static_cast<unsigned long long>(m.repeat_last_n);
  effective["draft_max"] = static_cast<unsigned long long>(m.draft_max);
  effective["draft_confidence"] = m.draft_confidence;
  effective["mtp_proposal_mode"] = m.mtp_proposal_mode;
  effective["prefill_batch"] = static_cast<unsigned long long>(m.prefill_batch);
  effective["context_lookup"] = m.context_lookup_enabled;
  effective["context"] = static_cast<unsigned long long>(m.context_capacity);
  v["effective_settings"] = std::move(effective);
  Value speed = Value::array();
  for (double x : m.speed_windows) speed.push_back(x);
  v["speed_windows_64_tokens"] = std::move(speed);
  Value rich = Value::array();
  for (const auto& w : m.request_windows) {
    Value item = Value::object();
    item["token_lo"] = static_cast<unsigned long long>(w.token_lo);
    item["token_hi"] = static_cast<unsigned long long>(w.token_hi);
    item["tokens_per_second"] = w.tokens_per_second;
    item["acceptance"] = w.acceptance;
    item["avg_draft_depth"] = w.avg_draft_depth;
    rich.push_back(std::move(item));
  }
  v["windows"] = std::move(rich);
  v["last_request_text"] = FormatLastRequest(m);
  if (m.profile.present) {
    const auto& p = m.profile;
    Value mtp = Value::object();
    mtp["draft_max"] = static_cast<unsigned long long>(p.draft_max);
    mtp["avg_draft_depth"] = p.mtp_avg_draft_depth;
    mtp["avg_cycle_draft_depth"] = p.avg_draft_depth;
    mtp["avg_speculative_draft_depth"] = p.avg_draft_depth;
    mtp["lookup_avg_draft_depth"] = p.lookup_avg_draft_depth;
    mtp["catchup_ms"] = p.catchup_ms;
    mtp["proposal_ms"] = p.proposal_ms;
    mtp["verification_ms"] = p.verification_ms;
    mtp["rollback_ms"] = p.rollback_ms;
    mtp["cycle_ms"] = p.mtp_cycle_ms;
    mtp["graph_captures"] = static_cast<unsigned long long>(p.graph_captures);
    mtp["mtp_cycles"] = static_cast<unsigned long long>(p.mtp_cycles);
    mtp["mtp_drafted"] = static_cast<unsigned long long>(p.mtp_drafted);
    mtp["mtp_accepted"] = static_cast<unsigned long long>(p.mtp_accepted);
    mtp["lookup_cycles"] = static_cast<unsigned long long>(p.lookup_cycles);
    mtp["lookup_drafted"] = static_cast<unsigned long long>(p.lookup_drafted);
    mtp["lookup_accepted"] = static_cast<unsigned long long>(p.lookup_accepted);
    Value mtp_economics = Value::object();
    mtp_economics["output_tokens"] = static_cast<unsigned long long>(p.mtp_output_tokens);
    mtp_economics["proposal_ms"] = p.mtp_proposal_ms;
    mtp_economics["verification_ms"] = p.mtp_verification_ms;
    mtp_economics["cycle_wall_ms"] = p.mtp_total_cycle_ms;
    mtp_economics["proposed_per_round"] = p.mtp_cycles ? static_cast<double>(p.mtp_drafted) / p.mtp_cycles : 0.0;
    mtp_economics["emitted_per_round"] = p.mtp_cycles ? static_cast<double>(p.mtp_output_tokens) / p.mtp_cycles : 0.0;
    mtp_economics["verify_ms_per_round"] = p.mtp_cycles ? p.mtp_verification_ms / p.mtp_cycles : 0.0;
    mtp_economics["verify_ms_per_output_token"] = p.mtp_output_tokens ? p.mtp_verification_ms / p.mtp_output_tokens : 0.0;
    mtp["mtp_economics"] = std::move(mtp_economics);
    Value lookup_economics = Value::object();
    lookup_economics["output_tokens"] = static_cast<unsigned long long>(p.lookup_output_tokens);
    lookup_economics["proposal_ms"] = p.lookup_proposal_ms;
    lookup_economics["verification_ms"] = p.lookup_verification_ms;
    lookup_economics["cycle_wall_ms"] = p.lookup_total_cycle_ms;
    lookup_economics["proposed_per_round"] = p.lookup_cycles ? static_cast<double>(p.lookup_drafted) / p.lookup_cycles : 0.0;
    lookup_economics["emitted_per_round"] = p.lookup_cycles ? static_cast<double>(p.lookup_output_tokens) / p.lookup_cycles : 0.0;
    lookup_economics["verify_ms_per_round"] = p.lookup_cycles ? p.lookup_verification_ms / p.lookup_cycles : 0.0;
    lookup_economics["verify_ms_per_output_token"] = p.lookup_output_tokens ? p.lookup_verification_ms / p.lookup_output_tokens : 0.0;
    mtp["lookup_economics"] = std::move(lookup_economics);
    const std::uint64_t profile_rounds = p.mtp_cycles + p.lookup_cycles;
    mtp["proposed_per_round"] = profile_rounds ? static_cast<double>(p.mtp_drafted + p.lookup_drafted) / static_cast<double>(profile_rounds) : 0.0;
    mtp["verify_ms_per_round"] = profile_rounds ? p.verification_ms / static_cast<double>(profile_rounds) : 0.0;
    mtp["verify_ms_per_output_token"] = m.completion_tokens ? p.verification_ms / static_cast<double>(m.completion_tokens) : 0.0;
    const auto survival_json = [](const std::uint64_t* proposed,
                                  const std::uint64_t* accepted) {
      Value survival = Value::array();
      for (int i = 0; i < fnvprof::kDepths - 1; ++i) {
        if (proposed[i] == 0) break;
        Value item = Value::object();
        item["position"] = i + 1;
        item["proposed"] = static_cast<unsigned long long>(proposed[i]);
        item["accepted"] = static_cast<unsigned long long>(accepted[i]);
        item["survival"] = static_cast<double>(accepted[i]) /
                           static_cast<double>(proposed[i]);
        survival.push_back(std::move(item));
      }
      return survival;
    };
    mtp["prefix_survival"] =
        survival_json(p.mtp_proposed_per_position, p.mtp_accepted_per_position);
    mtp["lookup_prefix_survival"] = survival_json(
        p.lookup_proposed_per_position, p.lookup_accepted_per_position);
    mtp["all_speculative_prefix_survival"] =
        survival_json(p.proposed_per_position, p.accepted_per_position);
    Value verification = Value::object();
    verification["trunk_ms"] = p.verify_trunk_ms;
    verification["hc_head_ms"] = p.verify_hc_head_ms;
    verification["vocab_projection_ms"] = p.verify_vocab_projection_ms;
    verification["compact_select_ms"] = p.verify_compact_select_ms;
    verification["d2h_ms"] = p.verify_d2h_ms;
    verification["cpu_accept_residual_ms"] = p.verify_cpu_accept_residual_ms;
    verification["gpu_events_reliable"] = p.verify_gpu_events_reliable;
    verification["gpu_events_incomplete"] = p.verify_gpu_events_incomplete;
    verification["output_rows"] = static_cast<unsigned long long>(p.verify_output_rows);
    verification["full_rows_d2h"] = static_cast<unsigned long long>(p.verify_full_rows_d2h);
    verification["compact_rows_d2h"] = static_cast<unsigned long long>(p.verify_compact_rows_d2h);
    verification["d2h_bytes"] = static_cast<unsigned long long>(p.verify_d2h_bytes);
    verification["compact_candidates"] = static_cast<unsigned long long>(p.verify_compact_candidates);
    verification["certificate_successes"] = static_cast<unsigned long long>(p.verify_certificate_successes);
    verification["certificate_fallbacks"] = static_cast<unsigned long long>(p.verify_certificate_fallbacks);
    verification["unsupported_sampler_fallbacks"] =
        static_cast<unsigned long long>(p.verify_unsupported_sampler_fallbacks);
    verification["rejection_frontier_downloads"] =
        static_cast<unsigned long long>(p.verify_rejection_frontier_downloads);
    mtp["verification"] = std::move(verification);
    const auto depth_json = [](const fnvprof::DepthRow* rows) {
      Value depths = Value::array();
      for (int depth = 0; depth < fnvprof::kDepths; ++depth) {
        const auto& row = rows[depth];
        Value item = Value::object();
        item["depth"] = depth;
        item["cycles"] = static_cast<unsigned long long>(row.cycles);
        item["avg_cycle_ms"] = row.cycles ? row.cycle_ms / static_cast<double>(row.cycles) : 0;
        item["drafted"] = static_cast<unsigned long long>(row.drafted);
        item["accepted"] = static_cast<unsigned long long>(row.accepted);
        item["acceptance"] = row.drafted ? static_cast<double>(row.accepted) / static_cast<double>(row.drafted) : 0;
        item["output_tokens"] = static_cast<unsigned long long>(row.output_tokens);
        item["output_tokens_per_second"] = row.cycle_ms > 0 ? 1000.0 * static_cast<double>(row.output_tokens) / row.cycle_ms : 0;
        item["proposal_ms_per_round"] = row.cycles ? row.proposal_ms / static_cast<double>(row.cycles) : 0.0;
        item["verify_ms_per_round"] = row.cycles ? row.verify_ms / static_cast<double>(row.cycles) : 0.0;
        item["verify_ms_per_output_token"] = row.output_tokens ? row.verify_ms / static_cast<double>(row.output_tokens) : 0.0;
        depths.push_back(std::move(item));
      }
      return depths;
    };
    mtp["depths"] = depth_json(p.depths);
    mtp["mtp_depths"] = depth_json(p.mtp_depths);
    mtp["lookup_depths"] = depth_json(p.lookup_depths);
    v["mtp"] = std::move(mtp);
    Value rich = Value::array();
    for (const auto& w : p.windows) {
      Value item = Value::object();
      item["token_lo"] = static_cast<unsigned long long>(w.token_lo);
      item["token_hi"] = static_cast<unsigned long long>(w.token_hi);
      item["tokens_per_second"] = w.tokens_per_second;
      item["acceptance"] = w.acceptance;
      item["avg_draft_depth"] = w.avg_draft_depth;
      item["proposal_ms"] = w.proposal_ms;
      item["verification_ms"] = w.verify_ms;
      item["qsa_ms"] = w.qsa_ms;
      item["ple_wait_ms"] = w.ple_wait_ms;
      rich.push_back(std::move(item));
    }
    v["profile_windows"] = std::move(rich);
    Value qsa = Value::object();
    qsa["indexer_ms"] = p.qsa_indexer_ms;
    qsa["selector_ms"] = p.qsa_selector_ms;
    qsa["sparse_attention_ms"] = p.qsa_sparse_ms;
    qsa["total_ms"] = p.qsa_total_ms;
    qsa["ms_per_output_token"] = m.completion_tokens ? p.qsa_total_ms / static_cast<double>(m.completion_tokens) : 0;
    qsa["pct_of_decode"] = m.decode_ms > 0 ? 100.0 * p.qsa_total_ms / m.decode_ms : 0;
    qsa["events_reliable"] = p.qsa_events_reliable;
    v["qsa"] = std::move(qsa);
    Value ple = Value::object();
    ple["read_count"] = static_cast<unsigned long long>(p.ple_reads);
    ple["disk_read_ms"] = p.ple_disk_ms;
    ple["wait_ms"] = p.ple_wait_ms;
    ple["avg_wait_ms"] = p.ple_avg_wait_ms;
    ple["max_wait_ms"] = p.ple_max_wait_ms;
    ple["ms_per_output_token"] = m.completion_tokens ? p.ple_wait_ms / static_cast<double>(m.completion_tokens) : 0;
    v["ple"] = std::move(ple);
    Value blas = Value::object();
    blas["tensile_libpath"] = p.tensile_libpath;
    blas["lt_calls"] = static_cast<unsigned long long>(p.lt_calls);
    blas["lt_success"] = static_cast<unsigned long long>(p.lt_success);
    blas["hipblas_fallback"] = static_cast<unsigned long long>(p.hipblas_fallback);
    blas["hipblas_direct"] = static_cast<unsigned long long>(p.hipblas_direct);
    blas["fallback_pct"] = p.fallback_pct;
    Value shapes = Value::array();
    for (const auto& shape : p.fallback_shapes) {
      Value item = Value::object();
      item["kind"] = shape.kind == 0 ? "fallback" : "direct";
      item["m"] = shape.m;
      item["n"] = shape.n;
      item["k"] = shape.k;
      item["type"] = shape.type;
      item["count"] = static_cast<unsigned long long>(shape.count);
      shapes.push_back(std::move(item));
    }
    blas["shapes"] = std::move(shapes);
    v["blas"] = std::move(blas);
    Value sync = Value::object();
    sync["stream_syncs"] = static_cast<unsigned long long>(p.stream_syncs);
    sync["device_syncs"] = static_cast<unsigned long long>(p.device_syncs);
    sync["event_syncs"] = static_cast<unsigned long long>(p.event_syncs);
    sync["wait_ms"] = p.sync_ms;
    sync["ms_per_output_token"] = m.completion_tokens ? p.sync_ms / static_cast<double>(m.completion_tokens) : 0;
    v["sync"] = std::move(sync);
    Value stages = Value::array();
    for (const auto& stage : p.stages) {
      Value item = Value::object();
      item["name"] = stage.name;
      item["ms"] = stage.ms;
      item["pct"] = stage.pct;
      stages.push_back(std::move(item));
    }
    v["stages"] = std::move(stages);
    v["profile_text"] = FormatProfile(m);
  }
  return v;
}

struct WindowsMemoryHeadroom {
  std::uint64_t available_physical{0};
  std::uint64_t available_commit{0};
};

WindowsMemoryHeadroom QueryWindowsMemoryHeadroom() {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) return {};
  return {static_cast<std::uint64_t>(status.ullAvailPhys),
          static_cast<std::uint64_t>(status.ullAvailPageFile)};
}

std::uint64_t WindowsAvailableMemoryBytes() {
  return QueryWindowsMemoryHeadroom().available_physical;
}


void LogDeviceMemory(std::string_view stage) {
  gufo::platform::DeviceMemorySnapshot memory{};
  const auto status = gufo::platform::QueryDeviceMemorySnapshot(&memory);
  if (status != hipSuccess) {
    std::cout << "Device memory " << stage << ": query failed ("
              << hipGetErrorString(status) << ")\n" << std::flush;
    return;
  }
  std::size_t effective_free = 0, effective_total = 0;
  const auto admission_status =
      gufo::platform::DeviceMemoryInfo(&effective_free, &effective_total);
  if (admission_status != hipSuccess) {
    std::cout << "Device memory " << stage << ": admission query failed ("
              << hipGetErrorString(admission_status) << ")\n" << std::flush;
    return;
  }
  std::cout << "Device memory " << stage
            << ": source=" << (memory.wddm ? "WDDM" : "HIP fallback")
            << " adapter_matched=" << memory.adapter_matched
            << " hip_free=" << memory.hip_free
            << " hip_total=" << memory.hip_total
            << " local_budget=" << memory.local_budget
            << " local_usage=" << memory.local_usage
            << " local_available=" << memory.local_available
            << " shared_budget=" << memory.shared_budget
            << " shared_usage=" << memory.shared_usage
            << " shared_available=" << memory.shared_available
            << " effective_free=" << effective_free
            << " effective_total=" << effective_total << " bytes\n"
            << std::flush;
}


class Runtime {
 public:
  explicit Runtime(Config cfg) : cfg_(std::move(cfg)) {
    LogDeviceMemory("before model load");
    CheckMemoryFloor("before model load");
    gufo::models::qwen38_flash_next::ModelOptions options;
    options.max_context = cfg_.context;
    options.mtp_model_path = cfg_.mtp;
    options.max_draft_tokens = cfg_.draft_max;
    options.prefill_batch = cfg_.prefill_batch;
    options.sampled_mtp_proposal_mode = ParseMtpProposalMode(cfg_.mtp_proposal_mode);
    options.vision_model_path = cfg_.mmproj;
    options.decode_concurrency = cfg_.sessions;
    options.context_lookup = cfg_.context_lookup;
    options.context_lookup_min_ngram = cfg_.context_lookup_min_ngram;
    options.context_lookup_max_ngram = cfg_.context_lookup_max_ngram;
    options.context_lookup_window = cfg_.context_lookup_window;
    options.context_lookup_min_draft = cfg_.context_lookup_min_draft;
    std::string error;
    const auto start = Clock::now();
    model_ = qfn::Load(cfg_.model, options, &error);
    if (!model_) throw std::runtime_error("model load failed: " + error);
    const auto mode = model_->HasMtp() ? gufo::core::SessionMode::kSpeculative
                                       : gufo::core::SessionMode::kAutoregressive;
    session_ = model_->CreateSession(mode, cfg_.context, &error);
    if (!session_) throw std::runtime_error("session creation failed: " + error);
    CheckMemoryFloor("after model load");
    LogDeviceMemory("after model load");
    load_seconds_ = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "Loaded " << model_->ModelName() << " in " << std::fixed << std::setprecision(2)
              << load_seconds_ << "s; resident " << (model_->ResidentBytes() / double(1ULL<<30))
              << " GiB; MTP=" << (model_->HasMtp() ? "on" : "off")
              << "; vision=" << (model_->VisionEncoder() ? "on" : "off") << "\n";
  }

  const Config& cfg() const noexcept { return cfg_; }
  std::shared_ptr<qfn> model() const noexcept { return model_; }
  Metrics last_metrics() const { std::lock_guard lock(metrics_mutex_); return last_; }
  double load_seconds() const noexcept { return load_seconds_; }

  struct Result {
    std::string raw;
    Metrics metrics;
  };

  Result Generate(std::span<const std::int32_t> prompt,
                  std::shared_ptr<const gufo::models::qwen::vision::Prompt> vision,
                  const gufo::sampling::SamplingConfig& sampling,
                  std::size_t max_tokens,
                  const std::function<void(std::string_view)>& delta = {},
                  bool profile = false,
                  bool thinking = false,
                  bool preserve_thinking = false,
                  std::string_view reasoning_effort = "OFF") {
    struct ProfileGuard {
      bool on{false};
      explicit ProfileGuard(bool enable) : on(enable) {
        if (!on) return;
        fnvprof::Reset();
        fnvprof::SetEnabled(true);
        fnvprof::SetPhase(fnvprof::Phase::Prefill);
      }
      ~ProfileGuard() {
        if (!on) return;
        fnvprof::SetPhase(fnvprof::Phase::Off);
        fnvprof::SetEnabled(false);
      }
    } profile_guard(profile);
    std::lock_guard generation_lock(generation_mutex_);
    CheckMemoryFloor("request start");
    if (!session_->IsValid()) {
      std::string error;
      const auto mode = model_->HasMtp() ? gufo::core::SessionMode::kSpeculative
                                         : gufo::core::SessionMode::kAutoregressive;
      session_ = model_->CreateSession(mode, cfg_.context, &error);
      if (!session_) throw std::runtime_error(error);
    }
    session_->ConfigureVision(std::move(vision));
    session_->ResetDraftPolicy();
    session_->SetDraftConfidence(cfg_.draft_confidence);
    const auto stats0 = session_->Statistics();
    const auto prefill0 = Clock::now();
    std::string error;
    if (!session_->Sync(prompt, &error)) throw std::runtime_error("prefill failed: " + error);
    const auto prefill1 = Clock::now();
    const auto context_capacity = session_->ContextSize();
    const auto context_depth = session_->Position();
    if (profile) {
      fnvprof::SetContext(context_capacity, context_depth);
      fnvprof::SetDraftMax(cfg_.draft_max);
      fnvprof::SetPhase(fnvprof::Phase::Decode);
    }

    auto history = ToSampler(prompt);
    gufo::sampling::SamplerState sampler(sampling, history);
    std::vector<std::int32_t> generated;
    generated.reserve(max_tokens);
    std::string stream_pending;
    auto decode0 = Clock::now();
    auto window_start = decode0;
    std::size_t window_tokens = 0;
    std::uint64_t window_drafted = 0;
    std::uint64_t window_accepted = 0;
    double window_depth = 0;
    std::uint32_t window_cycles = 0;
    std::uint32_t emitted = 0;
    std::vector<double> windows;
    std::vector<Metrics::Window> request_windows;
    bool stop = false;
    bool saw_token = false;
    double ttft_ms = 0;
    double depth_sum = 0;
    std::uint32_t depth_cycles = 0;
    auto close_window = [&](Clock::time_point now) {
      if (window_tokens == 0) return;
      Metrics::Window row;
      row.token_lo = emitted - static_cast<std::uint32_t>(window_tokens) + 1;
      row.token_hi = emitted;
      const double sec = std::chrono::duration<double>(now - window_start).count();
      row.tokens_per_second = sec > 0 ? static_cast<double>(window_tokens) / sec : 0;
      row.acceptance = window_drafted ? static_cast<double>(window_accepted) / static_cast<double>(window_drafted) : 0;
      row.avg_draft_depth = window_cycles ? window_depth / static_cast<double>(window_cycles) : 0;
      request_windows.push_back(row);
      if (window_tokens >= 64 && sec > 0) windows.push_back(row.tokens_per_second);
      window_tokens = 0;
      window_drafted = 0;
      window_accepted = 0;
      window_depth = 0;
      window_cycles = 0;
      window_start = now;
    };
    while (!stop && generated.size() < max_tokens) {
      if ((generated.size() & 31U) == 0) CheckMemoryFloor("generation");
      QfnSession::DecodeResult step;
      const auto remaining = max_tokens - generated.size();
      if (!session_->DecodeStep(remaining, sampler, &step, &error, true))
        throw std::runtime_error("decode failed: " + error);
      if (step.tokens.empty() && !step.stop) throw std::runtime_error("decode made no progress");
      generated.insert(generated.end(), step.tokens.begin(), step.tokens.end());
      if (!saw_token && !step.tokens.empty()) {
        ttft_ms = std::chrono::duration<double, std::milli>(Clock::now() - prefill0).count();
        if (profile) fnvprof::SetTtft(ttft_ms);
        saw_token = true;
      }
      stop = step.stop;
      if (!step.tokens.empty()) {
        emitted += static_cast<std::uint32_t>(step.tokens.size());
        window_tokens += step.tokens.size();
        window_drafted += step.drafted;
        window_accepted += step.accepted;
        window_depth += step.depth;
        ++window_cycles;
        depth_sum += step.depth;
        ++depth_cycles;
        if (window_tokens >= 64) close_window(Clock::now());
      }
      if (delta && !step.tokens.empty()) {
        stream_pending += model_->Decode(step.tokens);
        const auto complete = CompleteUtf8Prefix(stream_pending);
        if (complete != 0) {
          delta(std::string_view(stream_pending).substr(0, complete));
          stream_pending.erase(0, complete);
        }
      }
    }
    close_window(Clock::now());
    if (delta && !stream_pending.empty()) delta(stream_pending);
    const auto decode1 = Clock::now();
    const auto stats1 = session_->Statistics();
    Metrics m;
    m.prompt_tokens = prompt.size();
    m.completion_tokens = generated.size();
    m.prefill_ms = std::chrono::duration<double, std::milli>(prefill1 - prefill0).count();
    m.decode_ms = std::chrono::duration<double, std::milli>(decode1 - decode0).count();
    m.prefill_tps = m.prefill_ms > 0 ? 1000.0 * m.prompt_tokens / m.prefill_ms : 0;
    m.decode_tps = m.decode_ms > 0 ? 1000.0 * m.completion_tokens / m.decode_ms : 0;
    m.draft_tokens = stats1.drafted - stats0.drafted;
    m.draft_accepted = stats1.accepted - stats0.accepted;
    m.draft_acceptance = m.draft_tokens ? double(m.draft_accepted) / m.draft_tokens : 0;
    m.ttft_ms = ttft_ms;
    m.avg_inter_token_ms = m.completion_tokens ? m.decode_ms / static_cast<double>(m.completion_tokens) : 0;
    m.context_capacity = context_capacity;
    m.context_depth = context_depth;
    m.draft_max = cfg_.draft_max;
    m.avg_draft_depth = depth_cycles ? depth_sum / static_cast<double>(depth_cycles) : 0;
    m.draft_confidence = cfg_.draft_confidence;
    m.mtp_proposal_mode = cfg_.mtp_proposal_mode;
    m.prefill_batch = cfg_.prefill_batch;
    m.thinking = thinking;
    m.preserve_thinking = thinking && preserve_thinking;
    m.reasoning_effort = thinking ? std::string(reasoning_effort) : "OFF";
    m.compact_verification_candidates = model_->HasMtp()
        ? static_cast<std::uint32_t>(
              gufo::models::qwen38_flash_next::
                  CompactMtpVerificationCandidateCount(sampler))
        : 0;
    m.temperature = sampling.temperature;
    m.top_p = sampling.top_p;
    m.top_k = sampling.top_k;
    m.min_p = sampling.min_p;
    m.repeat_penalty = sampling.repeat_penalty;
    m.repeat_last_n = sampling.repeat_last_n;
    m.low_confidence_stops = session_->Statistics().low_confidence_stops - stats0.low_confidence_stops;
    m.compact_frontier_samples = session_->Statistics().compact_frontier_samples - stats0.compact_frontier_samples;
    m.compact_frontier_fallbacks = session_->Statistics().compact_frontier_fallbacks - stats0.compact_frontier_fallbacks;
    m.retained_frontier_downloads = session_->Statistics().retained_frontier_downloads - stats0.retained_frontier_downloads;
    m.async_halo_chains = session_->Statistics().async_halo_chains - stats0.async_halo_chains;
    m.async_halo_draft_tokens = session_->Statistics().async_halo_draft_tokens - stats0.async_halo_draft_tokens;
    m.mtp_fresh_resets = session_->MtpFreshResets();
    m.mtp_drafted = stats1.mtp_drafted - stats0.mtp_drafted;
    m.mtp_accepted = stats1.mtp_accepted - stats0.mtp_accepted;
    const auto mtp_conf_samples = stats1.mtp_confidence_samples - stats0.mtp_confidence_samples;
    const auto mtp_conf_sum = stats1.mtp_confidence_sum - stats0.mtp_confidence_sum;
    m.mtp_avg_proposal_confidence = mtp_conf_samples ? mtp_conf_sum / static_cast<double>(mtp_conf_samples) : 0.0;
    m.lookup_cycles = stats1.lookup_cycles - stats0.lookup_cycles;
    m.lookup_drafted = stats1.lookup_drafted - stats0.lookup_drafted;
    m.lookup_accepted = stats1.lookup_accepted - stats0.lookup_accepted;
    m.context_lookup_enabled = cfg_.context_lookup && model_->HasMtp();
    const auto lookup_ngram_sum = stats1.lookup_ngram_sum - stats0.lookup_ngram_sum;
    const auto lookup_distance_sum = stats1.lookup_distance_sum - stats0.lookup_distance_sum;
    m.lookup_avg_ngram = m.lookup_cycles ? static_cast<double>(lookup_ngram_sum) / static_cast<double>(m.lookup_cycles) : 0.0;
    m.lookup_avg_distance = m.lookup_cycles ? static_cast<double>(lookup_distance_sum) / static_cast<double>(m.lookup_cycles) : 0.0;
    const auto memory_headroom = QueryWindowsMemoryHeadroom();
    m.memory_available_bytes = memory_headroom.available_physical;
    m.memory_commit_available_bytes = memory_headroom.available_commit;
    m.request_windows = std::move(request_windows);
    m.speed_windows = std::move(windows);
    m.has_request = true;
    if (profile) {
      fnvprof::SetPhase(fnvprof::Phase::Off);
      m.profile = fnvprof::BuildReport(m.decode_ms);
    }
    {
      std::lock_guard lock(metrics_mutex_);
      last_ = m;
    }
    return {model_->Decode(generated), m};
  }

 private:
  [[nodiscard]] std::uint64_t MemoryFloorBytes() const noexcept {
    return static_cast<std::uint64_t>(cfg_.memory_guard_min_available_gib *
                                      static_cast<double>(1ULL << 30));
  }
  void CheckMemoryFloor(std::string_view stage) const {
    if (!cfg_.memory_guard) return;
    const auto memory = QueryWindowsMemoryHeadroom();
    const auto floor = MemoryFloorBytes();
    const bool physical_low =
        memory.available_physical != 0 && memory.available_physical < floor;
    const bool commit_low =
        memory.available_commit != 0 && memory.available_commit < floor;
    if (physical_low || commit_low) {
      std::ostringstream message;
      message << "Windows memory guard stopped " << stage << ": "
              << std::fixed << std::setprecision(2)
              << (memory.available_physical / static_cast<double>(1ULL << 30))
              << " GiB physical available, "
              << (memory.available_commit / static_cast<double>(1ULL << 30))
              << " GiB commit headroom; floor is "
              << cfg_.memory_guard_min_available_gib << " GiB";
      throw std::runtime_error(message.str());
    }
  }

  Config cfg_;
  std::shared_ptr<qfn> model_;
  std::unique_ptr<QfnSession> session_;
  mutable std::mutex generation_mutex_;
  mutable std::mutex metrics_mutex_;
  Metrics last_;
  double load_seconds_{0};
};

bool SendAll(SOCKET socket, std::string_view data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const int n = ::send(socket, data.data() + offset,
                         static_cast<int>(std::min<std::size_t>(data.size() - offset, 1 << 20)), 0);
    if (n <= 0) return false;
    offset += static_cast<std::size_t>(n);
  }
  return true;
}

std::string Reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default: return "Error";
  }
}

void SendResponse(SOCKET client, const HttpResponse& r) {
  std::ostringstream head;
  head << "HTTP/1.1 " << r.status << ' ' << Reason(r.status) << "\r\n"
       << "Content-Type: " << r.content_type << "\r\n"
       << "Content-Length: " << r.body.size() << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
       << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
       << "Connection: close\r\n\r\n";
  SendAll(client, head.str());
  SendAll(client, r.body);
}

void StartSse(SOCKET client) {
  const std::string headers =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream; charset=utf-8\r\n"
      "Cache-Control: no-cache\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Connection: close\r\n\r\n";
  SendAll(client, headers);
}

void Sse(SOCKET client, const Value& v) {
  const std::string line = "data: " + v.dump() + "\n\n";
  SendAll(client, line);
}

std::optional<HttpRequest> ReadRequest(SOCKET client) {
  std::string data;
  data.reserve(8192);
  char buf[4096];
  while (data.find("\r\n\r\n") == std::string::npos) {
    const int n = recv(client, buf, sizeof(buf), 0);
    if (n <= 0) return std::nullopt;
    data.append(buf, n);
    if (data.size() > 16 * 1024 * 1024) return std::nullopt;
  }
  const auto header_end = data.find("\r\n\r\n");
  const std::string header = data.substr(0, header_end);
  std::istringstream input(header);
  HttpRequest r;
  std::string version;
  input >> r.method >> r.path >> version;
  std::string line;
  std::size_t length = 0;
  std::getline(input, line);
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const auto name = Lower(Trim(std::string_view(line).substr(0, colon)));
    const auto value = Trim(std::string_view(line).substr(colon + 1));
    if (name == "content-length") length = static_cast<std::size_t>(std::stoull(value));
  }
  r.body = data.substr(header_end + 4);
  while (r.body.size() < length) {
    const int n = recv(client, buf, sizeof(buf), 0);
    if (n <= 0) return std::nullopt;
    r.body.append(buf, n);
    if (r.body.size() > 16 * 1024 * 1024) return std::nullopt;
  }
  if (r.body.size() > length) r.body.resize(length);
  const auto query = r.path.find('?');
  if (query != std::string::npos) r.path.resize(query);
  return r;
}

Value ErrorBody(std::string message) {
  Value root = Value::object();
  Value error = Value::object();
  error["message"] = std::move(message);
  error["type"] = "server_error";
  root["error"] = std::move(error);
  return root;
}

Value UsageJson(const Metrics& m) {
  Value usage = Value::object();
  usage["prompt_tokens"] = static_cast<unsigned long long>(m.prompt_tokens);
  usage["completion_tokens"] = static_cast<unsigned long long>(m.completion_tokens);
  usage["total_tokens"] = static_cast<unsigned long long>(m.prompt_tokens + m.completion_tokens);
  usage["flashnext_velocity"] = MetricsJson(m);
  return usage;
}

std::shared_ptr<const gufo::models::qwen::vision::Prompt> PrepareChatPrompt(
    Runtime& runtime, const ChatInput& chat, std::vector<std::int32_t>* tokens) {
  gufo::tokenization::ChatTemplateOptions options;
  options.add_generation_prompt = true;
  options.enable_thinking = chat.enable_thinking;
  options.reasoning_effort = chat.reasoning_effort;
  options.preserve_thinking = chat.preserve_thinking;
  options.add_vision_id = chat.add_vision_id;
  options.require_tool_call = chat.tool_required;
  const bool has_images = std::any_of(chat.messages.begin(), chat.messages.end(),
      [](const auto& m) { return !m.images.empty(); });
  if (has_images) {
    const auto& encoder = runtime.model()->VisionEncoder();
    if (!encoder) throw std::invalid_argument("image input requires mmproj");
    auto prepared = std::make_shared<gufo::models::qwen::vision::Prompt>(
        gufo::models::qwen::vision::Prepare(runtime.model()->tokenizer(), chat.messages,
                                             chat.tools, options, encoder->identity(),
                                             runtime.cfg().context));
    *tokens = ToI32(prepared->tokens);
    return prepared;
  }
  std::string error;
  const auto rendered = gufo::tokenization::QwenChatTemplate::RenderAndTokenize(
      runtime.model()->tokenizer(), chat.messages, chat.tools, options, &error);
  if (!rendered) throw std::invalid_argument(error);
  *tokens = ToI32(*rendered);
  return nullptr;
}

void HandleChat(SOCKET client, Runtime& runtime, const Value& body) {
  const auto chat = ParseChat(body, runtime.cfg());
  const auto sampling = ParseSampling(body, runtime.cfg());
  const auto max_tokens = MaxTokens(body, runtime.cfg());
  const bool stream = body.find("stream") && body.find("stream")->is_bool() && body.find("stream")->as_bool();
  const bool profile = body.find("flashnext_profile") && body.find("flashnext_profile")->is_bool() && body.find("flashnext_profile")->as_bool();
  const bool include_usage = body.find("stream_options") && body.find("stream_options")->is_object() &&
      body.find("stream_options")->find("include_usage") && body.find("stream_options")->find("include_usage")->is_bool() &&
      body.find("stream_options")->find("include_usage")->as_bool();
  std::vector<std::int32_t> prompt;
  auto vision = PrepareChatPrompt(runtime, chat, &prompt);
  const std::string id = RandomId("chatcmpl-");

  if (stream && chat.tools.empty() && !chat.enable_thinking) {
    StartSse(client);
    Value first = Value::object();
    first["id"] = id; first["object"] = "chat.completion.chunk"; first["model"] = runtime.model()->ModelName();
    Value choices = Value::array(); Value choice = Value::object(); choice["index"] = 0;
    Value delta = Value::object(); delta["role"] = "assistant"; choice["delta"] = delta; choice["finish_reason"] = Value(); choices.push_back(choice); first["choices"] = choices;
    Sse(client, first);
    auto result = runtime.Generate(prompt, vision, sampling, max_tokens,
      [&](std::string_view text) {
        Value chunk = Value::object(); chunk["id"] = id; chunk["object"] = "chat.completion.chunk"; chunk["model"] = runtime.model()->ModelName();
        Value cs = Value::array(); Value c = Value::object(); c["index"] = 0; Value d = Value::object(); d["content"] = std::string(text); c["delta"] = d; c["finish_reason"] = Value(); cs.push_back(c); chunk["choices"] = cs; Sse(client, chunk);
      }, profile, chat.enable_thinking,
      chat.enable_thinking && chat.preserve_thinking,
      EffortName(chat.reasoning_effort));
    Value end = Value::object(); end["id"] = id; end["object"] = "chat.completion.chunk"; end["model"] = runtime.model()->ModelName();
    Value cs = Value::array(); Value c = Value::object(); c["index"] = 0; c["delta"] = Value::object(); c["finish_reason"] = "stop"; cs.push_back(c); end["choices"] = cs;
    if (include_usage) end["usage"] = UsageJson(result.metrics);
    Sse(client, end);
    SendAll(client, "data: [DONE]\n\n");
    return;
  }

  // Tool calls and reasoning are buffered so the final OpenAI structure is
  // correct rather than leaking template control tokens into content.
  const auto result = runtime.Generate(
      prompt, vision, sampling, max_tokens, {}, profile, chat.enable_thinking,
      chat.enable_thinking && chat.preserve_thinking,
      EffortName(chat.reasoning_effort));
  const auto parsed = ParseOutput(result.raw, chat.enable_thinking, !chat.tools.empty());
  if (chat.tool_required && parsed.calls.empty())
    throw std::runtime_error("tool_choice=required but the model did not produce a tool call");

  if (stream) {
    StartSse(client);
    Value chunk = Value::object(); chunk["id"] = id; chunk["object"] = "chat.completion.chunk"; chunk["model"] = runtime.model()->ModelName();
    Value cs = Value::array(); Value c = Value::object(); c["index"] = 0; Value d = Value::object(); d["role"] = "assistant";
    if (!parsed.reasoning.empty()) d["reasoning_content"] = parsed.reasoning;
    if (!parsed.content.empty()) d["content"] = parsed.content;
    if (!parsed.calls.empty()) {
      Value arr = Value::array();
      for (std::size_t i = 0; i < parsed.calls.size(); ++i) {
        Value tc = Value::object(); tc["index"] = i; tc["id"] = parsed.calls[i].id; tc["type"] = "function";
        Value fn = Value::object(); fn["name"] = parsed.calls[i].name; fn["arguments"] = parsed.calls[i].arguments.dump(); tc["function"] = fn; arr.push_back(tc);
      }
      d["tool_calls"] = arr;
    }
    c["delta"] = d; c["finish_reason"] = parsed.calls.empty() ? "stop" : "tool_calls"; cs.push_back(c); chunk["choices"] = cs;
    if (include_usage) chunk["usage"] = UsageJson(result.metrics);
    Sse(client, chunk); SendAll(client, "data: [DONE]\n\n");
    return;
  }

  Value root = Value::object(); root["id"] = id; root["object"] = "chat.completion"; root["model"] = runtime.model()->ModelName();
  Value choices = Value::array(); Value choice = Value::object(); choice["index"] = 0;
  Value msg = Value::object(); msg["role"] = "assistant"; msg["content"] = parsed.content.empty() ? Value() : Value(parsed.content);
  if (!parsed.reasoning.empty()) msg["reasoning_content"] = parsed.reasoning;
  if (!parsed.calls.empty()) {
    Value arr = Value::array();
    for (const auto& call : parsed.calls) {
      Value tc = Value::object(); tc["id"] = call.id; tc["type"] = "function";
      Value fn = Value::object(); fn["name"] = call.name; fn["arguments"] = call.arguments.dump(); tc["function"] = fn; arr.push_back(tc);
    }
    msg["tool_calls"] = arr;
  }
  choice["message"] = msg; choice["finish_reason"] = parsed.calls.empty() ? "stop" : "tool_calls"; choices.push_back(choice); root["choices"] = choices;
  root["usage"] = UsageJson(result.metrics);
  SendResponse(client, {200, "application/json", root.dump()});
}

void HandleCompletion(SOCKET client, Runtime& runtime, const Value& body) {
  const auto prompt = body.member_str("prompt");
  if (prompt.empty()) throw std::invalid_argument("prompt must be a non-empty string");
  const auto sampling = ParseSampling(body, runtime.cfg());
  const auto max_tokens = MaxTokens(body, runtime.cfg());
  auto tokens = runtime.model()->Tokenize(prompt);
  const bool profile = body.find("flashnext_profile") && body.find("flashnext_profile")->is_bool() && body.find("flashnext_profile")->as_bool();
  const auto result = runtime.Generate(tokens, nullptr, sampling, max_tokens, {}, profile);
  Value root = Value::object(); root["id"] = RandomId("cmpl-"); root["object"] = "text_completion"; root["model"] = runtime.model()->ModelName();
  Value choices = Value::array(); Value c = Value::object(); c["index"] = 0; c["text"] = result.raw; c["finish_reason"] = "stop"; choices.push_back(c); root["choices"] = choices; root["usage"] = UsageJson(result.metrics);
  SendResponse(client, {200, "application/json", root.dump()});
}


std::string DashboardHtml(const Runtime& runtime) {
  std::ostringstream html;
  html << R"FNVHTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlashNextVelocity</title>
<style>
:root{color-scheme:dark;font-family:Segoe UI,Arial,sans-serif;background:#0f1419;color:#e7edf3}body{margin:0;background:#0f1419}header{padding:18px 24px;background:#121b24;border-bottom:1px solid #263647;display:flex;justify-content:space-between;align-items:center}.brand{font-size:22px;font-weight:700}.muted{color:#9db0c2}.wrap{max-width:1180px;margin:0 auto;padding:22px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px}.card{background:#151f29;border:1px solid #263647;border-radius:12px;padding:16px;box-shadow:0 3px 14px #0004}.label{font-size:12px;color:#9db0c2;text-transform:uppercase;letter-spacing:.08em}.value{font-size:20px;margin-top:6px;word-break:break-word}.ok{color:#72e0a0}.bad{color:#ff8585}.row{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}button,input,textarea,select{background:#0d151d;color:#edf4fa;border:1px solid #33485c;border-radius:8px;padding:10px 12px;font:inherit}button{cursor:pointer;background:#18344a}button:hover{background:#21465f}textarea{width:100%;min-height:110px;box-sizing:border-box;resize:vertical}.wide{grid-column:1/-1}pre{white-space:pre-wrap;background:#0b1117;border:1px solid #263647;border-radius:8px;padding:12px;min-height:80px;overflow:auto}.pill{padding:4px 9px;border-radius:999px;background:#18344a;font-size:12px}.metric{font-size:28px;font-weight:700}.small{font-size:13px}a{color:#7cc7ff}.two{display:grid;grid-template-columns:1fr 1fr;gap:14px}@media(max-width:800px){.two{grid-template-columns:1fr}}
</style></head><body>
<header><div><div class="brand">FlashNextVelocity</div><div class="muted small">Native Windows Strix Halo inference</div></div><div class="pill" id="statusPill">checking…</div></header>
<div class="wrap">
<div class="grid">
<div class="card"><div class="label">Model</div><div class="value" id="model">-</div></div>
<div class="card"><div class="label">MTP</div><div class="value" id="mtp">-</div></div>
<div class="card"><div class="label">Vision</div><div class="value" id="vision">-</div></div>
<div class="card"><div class="label">API Base</div><div class="value small" id="api">/v1</div></div>
<div class="card wide"><div class="label">Quick Chat</div><textarea id="prompt">Write a short Python function that returns the nth Fibonacci number.</textarea><div class="row"><input id="maxTokens" type="number" value="256" min="1" max="8192"><button onclick="chat()">Send</button><button onclick="clearOut()">Clear</button></div><pre id="chatOut"></pre></div>
<div class="card wide"><div class="label">Benchmark</div><div class="row"><input id="benchTokens" type="number" value="512" min="64" max="4096"><button onclick="bench()">Run benchmark</button></div><div class="grid" style="margin-top:12px"><div><div class="label">Prefill</div><div class="metric" id="prefill">-</div></div><div><div class="label">Decode</div><div class="metric" id="decode">-</div></div><div><div class="label">MTP Accept</div><div class="metric" id="accept">-</div></div></div><pre id="benchOut"></pre></div>
<div class="card wide"><div class="label">Last Metrics</div><pre id="metrics">-</pre></div>
</div></div>
<script>
const base=location.origin;
async function refresh(){try{const h=await fetch(base+'/health').then(r=>r.json());model.textContent=h.model||'-';mtp.textContent=h.mtp?('ON · '+(h.mtp_proposal_mode||'halo_greedy')+' · lookup '+(h.context_lookup_active?'ON':'OFF')):'OFF';vision.textContent=h.vision?'ON':'OFF';statusPill.textContent='online';statusPill.className='pill ok';api.textContent=base+'/v1';const m=await fetch(base+'/metrics').then(r=>r.json());metrics.textContent=JSON.stringify(m,null,2)}catch(e){statusPill.textContent='offline';statusPill.className='pill bad'}}
async function studioSettings(){const h=await fetch(base+'/health').then(r=>r.json());const s=h.sampling||{};return{temperature:s.temperature,top_p:s.top_p,top_k:s.top_k,min_p:s.min_p,repeat_penalty:s.repeat_penalty,repeat_last_n:s.repeat_last_n,chat_template_kwargs:{enable_thinking:!!h.thinking,preserve_thinking:!!h.thinking&&!!h.preserve_thinking,reasoning_effort:h.reasoning_effort||'medium'}}}
async function chat(){chatOut.textContent='working…';try{const settings=await studioSettings();const body={model:'local',messages:[{role:'user',content:prompt.value}],max_tokens:+maxTokens.value,stream:false,...settings};const r=await fetch(base+'/v1/chat/completions',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const j=await r.json();if(!r.ok)throw new Error(j?.error?.message||r.statusText);chatOut.textContent=j.choices?.[0]?.message?.content||JSON.stringify(j,null,2);refresh()}catch(e){chatOut.textContent='ERROR: '+e.message}}
function clearOut(){chatOut.textContent=''}
async function bench(){benchOut.textContent='warming up…';try{const settings=await studioSettings();await fetch(base+'/v1/chat/completions',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({model:'local',messages:[{role:'user',content:'Reply with one short sentence about speculative decoding.'}],max_tokens:24,stream:false,...settings})});benchOut.textContent='benchmarking…';const longPrompt=('Explain speculative decoding, memory bandwidth, recurrent state, and long-context inference in technically precise, non-repetitive prose. ').repeat(48);const r=await fetch(base+'/v1/chat/completions',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({model:'local',messages:[{role:'user',content:longPrompt}],max_tokens:+benchTokens.value,stream:false,flashnext_profile:true,...settings})});const j=await r.json();if(!r.ok)throw new Error(j?.error?.message||r.statusText);const m=j.usage?.flashnext_velocity||{};prefill.textContent=(m.prefill_tokens_per_second||0).toFixed(2)+' tok/s';decode.textContent=(m.completion_tokens_per_second||0).toFixed(2)+' tok/s';accept.textContent=(((m.mtp_acceptance!==undefined?m.mtp_acceptance:m.draft_acceptance)||0)*100).toFixed(1)+'%';benchOut.textContent=m.profile_text||('64-token windows: '+JSON.stringify(m.speed_windows_64_tokens||[])+'\nDraft: '+(m.draft_tokens_accepted||0)+'/'+(m.draft_tokens||0));refresh()}catch(e){benchOut.textContent='ERROR: '+e.message}}
refresh();setInterval(refresh,5000);
</script></body></html>)FNVHTML";
  return html.str();
}

void HandleConnection(SOCKET client, Runtime& runtime) {
  try {
    const auto request = ReadRequest(client);
    if (!request) { closesocket(client); return; }
    if (request->method == "OPTIONS") { SendResponse(client, {200,"text/plain",""}); closesocket(client); return; }
    if (request->method == "GET" && (request->path == "/" || request->path == "/dashboard")) {
      SendResponse(client, {200,"text/html; charset=utf-8",DashboardHtml(runtime)}); closesocket(client); return;
    }
    if (request->method == "GET" && request->path == "/v1") {
      Value v = Value::object(); v["name"] = "FlashNextVelocity"; v["version"] = "1.0.10";
      Value endpoints = Value::array(); endpoints.push_back("/v1/models"); endpoints.push_back("/v1/chat/completions"); endpoints.push_back("/v1/completions"); endpoints.push_back("/health"); endpoints.push_back("/metrics"); endpoints.push_back("/dashboard"); v["endpoints"] = endpoints;
      SendResponse(client, {200,"application/json",v.dump()}); closesocket(client); return;
    }
    if (request->method == "GET" && request->path == "/health") {
      Value v = Value::object();
      v["status"] = "ok";
      v["version"] = "1.0.10";
      v["model"] = runtime.model()->ModelName();
      v["mtp"] = runtime.model()->HasMtp();
      v["vision"] = static_cast<bool>(runtime.model()->VisionEncoder());
      v["load_seconds"] = runtime.load_seconds();
      // Expose the exact startup configuration so the Studio can verify that
      // Save/Restart launched the engine from the same dist\config.json the UI
      // just persisted. These are local-only diagnostics, not request overrides.
      v["runtime_revision"] = std::string(kRuntimeRevision);
      v["config_model"] = runtime.cfg().model;
      v["config_mtp"] = runtime.cfg().mtp;
      v["config_mmproj"] = runtime.cfg().mmproj;
      v["host"] = runtime.cfg().host;
      v["port"] = runtime.cfg().port;
      v["context"] = static_cast<unsigned long long>(runtime.cfg().context);
      v["draft_max"] = static_cast<unsigned long long>(runtime.cfg().draft_max);
      v["draft_confidence"] = runtime.cfg().draft_confidence;
      v["mtp_proposal_mode"] = runtime.cfg().mtp_proposal_mode;
      v["mtp_hidden_normalization"] = "per_hc_stream";
      v["mtp_async_halo_chain_fast_path"] = true;
      v["verification_frontier_transfer"] = "gpu_retained_lazy_d2h";
      v["prefill_batch"] = static_cast<unsigned long long>(runtime.cfg().prefill_batch);
      v["context_lookup"] = runtime.cfg().context_lookup;
      v["context_lookup_active"] = runtime.cfg().context_lookup && runtime.model()->HasMtp();
      v["context_lookup_min_ngram"] = static_cast<unsigned long long>(runtime.cfg().context_lookup_min_ngram);
      v["context_lookup_max_ngram"] = static_cast<unsigned long long>(runtime.cfg().context_lookup_max_ngram);
      v["context_lookup_window"] = static_cast<unsigned long long>(runtime.cfg().context_lookup_window);
      v["context_lookup_min_draft"] = static_cast<unsigned long long>(runtime.cfg().context_lookup_min_draft);
      v["memory_guard"] = runtime.cfg().memory_guard;
      v["memory_guard_min_available_gib"] = runtime.cfg().memory_guard_min_available_gib;
      const auto memory = QueryWindowsMemoryHeadroom();
      v["memory_available_bytes"] = static_cast<unsigned long long>(memory.available_physical);
      v["memory_commit_available_bytes"] = static_cast<unsigned long long>(memory.available_commit);
      v["sessions"] = static_cast<unsigned long long>(runtime.cfg().sessions);
      v["default_max_tokens"] = static_cast<unsigned long long>(runtime.cfg().default_max_tokens);
      v["thinking"] = runtime.cfg().thinking;
      v["preserve_thinking"] = runtime.cfg().preserve_thinking;
      v["reasoning_effort"] = runtime.cfg().reasoning_effort;
      Value sampling = Value::object();
      sampling["temperature"] = runtime.cfg().sampling.temperature;
      sampling["top_p"] = runtime.cfg().sampling.top_p;
      sampling["top_k"] = runtime.cfg().sampling.top_k;
      sampling["min_p"] = runtime.cfg().sampling.min_p;
      sampling["repeat_penalty"] = runtime.cfg().sampling.repeat_penalty;
      sampling["repeat_last_n"] = static_cast<unsigned long long>(runtime.cfg().sampling.repeat_last_n);
      v["sampling"] = std::move(sampling);
      SendResponse(client, {200,"application/json",v.dump()}); closesocket(client); return;
    }
    if (request->method == "GET" && request->path == "/metrics") {
      SendResponse(client, {200,"application/json",MetricsJson(runtime.last_metrics()).dump()}); closesocket(client); return;
    }
    if (request->method == "GET" && request->path == "/v1/models") {
      Value root = Value::object(); root["object"] = "list"; Value data = Value::array(); Value model = Value::object(); model["id"] = runtime.model()->ModelName(); model["object"] = "model"; data.push_back(model); root["data"] = data;
      SendResponse(client, {200,"application/json",root.dump()}); closesocket(client); return;
    }
    if (request->method != "POST") { SendResponse(client, {405,"application/json",ErrorBody("method not allowed").dump()}); closesocket(client); return; }
    const Value body = gufo::json::parse(request->body);
    if (request->path == "/v1/chat/completions") HandleChat(client, runtime, body);
    else if (request->path == "/v1/completions") HandleCompletion(client, runtime, body);
    else SendResponse(client, {404,"application/json",ErrorBody("not found").dump()});
  } catch (const std::exception& e) {
    SendResponse(client, {400,"application/json",ErrorBody(e.what()).dump()});
  }
  closesocket(client);
}

class Winsock {
 public:
  Winsock() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2), &data) != 0) throw std::runtime_error("WSAStartup failed");
  }
  ~Winsock() { WSACleanup(); }
};

void Serve(Runtime& runtime) {
  Winsock winsock;
  SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server == INVALID_SOCKET) throw std::runtime_error("socket() failed");
  BOOL yes = TRUE;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u_short>(runtime.cfg().port));
  if (InetPtonA(AF_INET, runtime.cfg().host.c_str(), &address.sin_addr) != 1) {
    closesocket(server); throw std::runtime_error("host must be an IPv4 address");
  }
  if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(server, SOMAXCONN) == SOCKET_ERROR) {
    closesocket(server); throw std::runtime_error("bind/listen failed (port already in use?)");
  }
  std::cout << "FlashNextVelocity native Gufo engine: http://" << runtime.cfg().host << ':' << runtime.cfg().port << "/v1\n";
  std::cout << "Health: http://" << runtime.cfg().host << ':' << runtime.cfg().port << "/health\n";
  for (;;) {
    SOCKET client = accept(server, nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;
    std::thread([client, &runtime] { HandleConnection(client, runtime); }).detach();
  }
}

void PrintUsage() {
  std::cout << "FlashNextVelocity --config <config.json>\n"
            << "Native Windows gfx1151 Qwen3.8 Flash-Next engine derived from Gufo.\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    fs::path config_path = "config.json";
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--config" && i + 1 < argc) config_path = argv[++i];
      else if (arg == "--help" || arg == "-h") { PrintUsage(); return 0; }
    }
    // Production enables only the Prompt 6 winner. An explicit environment
    // value (including "none" for A/B validation) always takes precedence.
    if (std::getenv("GUFO_PLATFORM_TUNING") == nullptr &&
        _putenv_s("GUFO_PLATFORM_TUNING", "none,+flag_waits") != 0)
      throw std::runtime_error("could not set Gufo platform tuning");
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error("curl_global_init failed");
    struct CurlCleanup { ~CurlCleanup(){ curl_global_cleanup(); } } curl_cleanup;
    Runtime runtime(LoadConfig(config_path));
    Serve(runtime);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FATAL: " << e.what() << '\n';
    return 1;
  }
}
