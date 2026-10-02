#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "src/core/json.hpp"

namespace fnvchat {
using gufo::json::Value;

struct ToolCall {
  std::string id;
  std::string name;
  Value arguments{Value::object()};
};
struct Output {
  std::string content;
  std::string reasoning;
  std::vector<ToolCall> calls;
  bool incomplete_tool{false};
};

inline std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == text.npos) return {};
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// One parser drives both streaming and non-streaming responses. Only complete
// tool blocks become structured calls; normal text (including SVG) is verbatim.
class Parser {
 public:
  using TextSink = std::function<void(std::string_view, bool)>;
  using CallSink = std::function<void(const ToolCall&, std::size_t)>;
  Parser(bool thinking, std::vector<Value> tools,
         std::function<std::string()> make_id, TextSink text = {}, CallSink call = {})
      : initial_(thinking), reasoning_(thinking), tools_(std::move(tools)), make_id_(std::move(make_id)),
        text_(std::move(text)), call_(std::move(call)) {}

  void Feed(std::string_view text) {
    pending_.append(text);
    Drain(false);
  }
  Output Finish() {
    Drain(true);
    output_.incomplete_tool = in_tool_;
    return output_;
  }

 private:
  void Emit(std::string_view text, bool reasoning) {
    if (text.empty()) return;
    (reasoning ? output_.reasoning : output_.content).append(text);
    if (text_) text_(text, reasoning);
  }

  // Retain a possible marker prefix across token/UTF-8 fragment boundaries.
  static std::size_t Suffix(std::string_view text, std::string_view marker) {
    for (auto n = std::min(text.size(), marker.size() - 1); n > 0; --n)
      if (text.ends_with(marker.substr(0, n))) return n;
    return 0;
  }

  Value Argument(std::string_view raw, const Value& schema) {
    const auto type = schema.member_str("type");
    // String arguments can themselves be valid JSON ("123", "true", code).
    // Never guess their type from their contents.
    if (type == "string") return std::string(raw);
    try { return gufo::json::parse(Trim(raw)); }
    catch (...) { return std::string(raw); }
  }

  ToolCall ParseCall(std::string_view body) {
    body = Trim(body);
    ToolCall result;
    if (body.starts_with("{")) {
      auto json = gufo::json::parse(body);
      result.name = json.member_str("name");
      if (const auto* args = json.find("arguments")) {
        result.arguments = args->is_string() ? gufo::json::parse(args->get_str()) : *args;
      }
    } else if (body.starts_with("<function=")) {
      const auto end = body.find('>');
      const auto close = body.rfind("</function>");
      if (end == body.npos || close == body.npos || close < end)
        throw std::runtime_error("malformed model tool call");
      result.name = std::string(Trim(body.substr(10, end - 10)));
      const Value* properties = nullptr;
      for (const auto& tool : tools_)
        if (tool.member_str("name") == result.name)
          if (const auto* parameters = tool.find("parameters"))
            properties = parameters->find("properties");
      auto pos = end + 1;
      while ((pos = body.find("<parameter=", pos)) != body.npos && pos < close) {
        const auto pe = body.find('>', pos);
        if (pe == body.npos) throw std::runtime_error("malformed tool parameter");
        const auto ce = body.find("</parameter>", pe + 1);
        if (ce == body.npos || ce > close) throw std::runtime_error("unterminated tool parameter");
        const auto name = std::string(Trim(body.substr(pos + 11, pe - pos - 11)));
        auto raw = body.substr(pe + 1, ce - pe - 1);
        // The template puts one formatting newline on either side of values.
        // Preserve interior/trailing whitespace in code and file contents.
        if (raw.starts_with("\r\n")) raw.remove_prefix(2);
        else if (raw.starts_with("\n")) raw.remove_prefix(1);
        if (raw.ends_with("\r\n")) raw.remove_suffix(2);
        else if (raw.ends_with("\n")) raw.remove_suffix(1);
        const auto* schema = properties ? properties->find(name) : nullptr;
        result.arguments[name] = Argument(raw, schema ? *schema : Value::object());
        pos = ce + 12;
      }
    }
    if (result.name.empty() || !result.arguments.is_object())
      throw std::runtime_error("model produced an invalid tool call");
    const auto tool = std::find_if(tools_.begin(), tools_.end(), [&](const auto& candidate) {
      return candidate.member_str("name") == result.name;
    });
    if (tool == tools_.end())
      throw std::runtime_error("model called an unavailable tool: " + result.name);
    if (const auto* parameters = tool->find("parameters")) {
      if (const auto* required = parameters->find("required"))
        for (const auto& key : required->items())
          if (!result.arguments.find(key.str()))
            throw std::runtime_error("model tool call " + result.name + " is missing required argument: " + key.str());
      if (const auto* properties = parameters->find("properties"))
        for (const auto& [key, value] : result.arguments.members()) {
          const auto* schema = properties->find(key);
          if (!schema) continue;
          const auto type = schema->member_str("type");
          if ((type == "string" && !value.is_string()) ||
              (type == "boolean" && !value.is_bool()) ||
              (type == "number" && !value.is_number()) ||
              (type == "integer" && (!value.is_number() || std::floor(value.as_double()) != value.as_double())) ||
              (type == "object" && !value.is_object()) ||
              (type == "array" && !value.is_array()) ||
              (type == "null" && !value.is_null()))
            throw std::runtime_error("model tool call " + result.name + " argument " + key + " must be " + type);
        }
    }
    result.id = make_id_();
    return result;
  }

  void Drain(bool final) {
    if (initial_) {
      if (!final && std::string_view("<think>").starts_with(pending_)) return;
      if (pending_.starts_with("<think>")) {
        pending_.erase(0, 7);
        reasoning_ = true;
      }
      initial_ = false;
    }
    while (!pending_.empty()) {
      if (in_tool_) {
        const auto end = pending_.find("</tool_call>");
        if (end == pending_.npos) {
          return;
        }
        auto tool = ParseCall(std::string_view(pending_).substr(0, end));
        output_.calls.push_back(std::move(tool));
        if (call_) call_(output_.calls.back(), output_.calls.size() - 1);
        pending_.erase(0, end + 12);
        in_tool_ = false;
        continue;
      }
      const std::string_view marker = reasoning_ ? "</think>" : "<tool_call>";
      const bool recognize = reasoning_ || !tools_.empty();
      const auto end = recognize ? pending_.find(marker) : pending_.npos;
      if (end != pending_.npos) {
        Emit(std::string_view(pending_).substr(0, end), reasoning_);
        pending_.erase(0, end + marker.size());
        if (reasoning_) reasoning_ = false;
        else in_tool_ = true;
        continue;
      }
      const auto hold = recognize && !final ? Suffix(pending_, marker) : 0;
      const auto count = pending_.size() - hold;
      Emit(std::string_view(pending_).substr(0, count), reasoning_);
      pending_.erase(0, count);
      return;
    }
  }

  bool initial_{true}, reasoning_{false}, in_tool_{false};
  std::string pending_;
  std::vector<Value> tools_;
  std::function<std::string()> make_id_;
  TextSink text_;
  CallSink call_;
  Output output_;
};
}  // namespace fnvchat
