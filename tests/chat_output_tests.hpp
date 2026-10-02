#pragma once
#include "chat_output.hpp"

static void TestChatOutput() {
  using fnvchat::Parser;
  using gufo::json::parse;
  const std::vector<gufo::json::Value> tools{
      parse(R"({"name":"write","parameters":{"properties":{"path":{"type":"string"},"content":{"type":"string"},"count":{"type":"integer"}},"required":["path","content"]}})")};
  const std::string source =
      "<think>Check the file.</think>Writing now.\n<tool_call>\n<function=write>\n"
      "<parameter=path>\n123\n</parameter>\n"
      "<parameter=content>\n  <svg>\n    true\n  </svg>  \n</parameter>\n"
      "<parameter=count>\n2\n</parameter>\n</function>\n</tool_call>";
  for (std::size_t width = 1; width <= source.size(); ++width) {
    std::string text, reasoning;
    std::size_t calls = 0;
    Parser p(true, tools, [] { return "call_test"; },
        [&](std::string_view s, bool thought) { (thought ? reasoning : text).append(s); },
        [&](const auto& call, auto index) {
          Check(index == calls++, "stream tool index mismatch");
          Check(call.name == "write", "wrong streamed tool name");
        });
    for (std::size_t i = 0; i < source.size(); i += width)
      p.Feed(std::string_view(source).substr(i, width));
    const auto out = p.Finish();
    Check(out.content == text && text == "Writing now.\n", "stream content or marker leakage");
    Check(out.reasoning == reasoning && reasoning == "Check the file.", "reasoning split mismatch");
    Check(calls == 1 && out.calls.size() == 1, "tool lost or emitted twice");
    const auto& args = out.calls[0].arguments;
    Check(args.member_str("path") == "123", "numeric string argument coerced");
    Check(args.member_str("content") == "  <svg>\n    true\n  </svg>  ", "file content whitespace lost");
    Check(args.find("count")->is_number(), "integer argument became a string");
  }
  {
    const std::string literal = "<think>literal XML</think>\n```xml\n<svg><text>&lt;hello&gt;</text></svg>\n```\n<tool_call>example</tool_call>";
    Parser p(false, {}, [] { return "id"; });
    for (const char c : literal) p.Feed(std::string_view(&c, 1));
    Check(p.Finish().content == literal, "tool-free SVG/text was modified");
  }
  {
    // Every interruption point in a real write call must publish zero calls,
    // including EOF immediately after the opening marker or function header.
    const auto tool_start = source.find("<tool_call>");
    const auto tool = std::string_view(source).substr(tool_start);
    for (std::size_t cut = std::string_view("<tool_call>").size(); cut < tool.size(); ++cut) {
      std::size_t calls = 0, ids = 0;
      Parser p(false, tools, [&] { ++ids; return "id"; }, {},
          [&](const auto&, auto) { ++calls; });
      p.Feed("Before ");
      p.Feed(tool.substr(0, cut));
      const auto out = p.Finish();
      Check(out.incomplete_tool && out.calls.empty() && out.content == "Before ", "partial tool handled as success");
      Check(calls == 0 && ids == 0, "interrupted write published a provisional call");
    }
  }
  {
    Parser p(false, tools, [] { return "id"; });
    p.Feed(R"(<tool_call>{"name":"write","arguments":{"path":"true","content":"SVG"}}</tool_call>)");
    Check(p.Finish().calls[0].arguments.member_str("path") == "true", "JSON tool parsing failed");
  }
  {
    bool failed = false;
    try {
      Parser p(false, tools, [] { return "id"; });
      p.Feed(R"(<tool_call>{"name":"invented","arguments":{}}</tool_call>)");
    } catch (const std::runtime_error&) { failed = true; }
    Check(failed, "invented tool accepted");
  }
  {
    std::size_t count = 0;
    Parser p(false, tools, [] { return "id"; }, {},
        [&](const auto&, auto index) { Check(index == count++, "parallel call index"); });
    p.Feed(R"(<tool_call>{"name":"write","arguments":{"path":"a","content":"A"}}</tool_call><tool_call>{"name":"write","arguments":{"path":"b","content":"B"}}</tool_call>)");
    Check(count == 2 && p.Finish().calls.size() == 2, "parallel calls missing");
  }
  {
    bool completed = false;
    Parser p(false, tools, [] { return "complete_id"; }, {},
        [&](const auto& call, auto index) {
          completed = true;
          Check(call.id == "complete_id" && call.name == "write" && index == 0, "complete tool metadata wrong");
          Check(call.arguments.member_str("content") == "SVG", "complete tool arguments missing");
        });
    p.Feed("<tool_call>\n<function=write>\n<parameter=path>\npelican.svg\n</parameter>\n<parameter=content>\n");
    Check(!completed, "write announced before arguments complete");
    p.Feed("SVG\n</parameter>\n</function>\n</tool_call>");
    Check(completed && p.Finish().calls.size() == 1, "complete call not emitted");
  }
  for (const auto* invalid : {
      R"(<tool_call>{"name":"write","arguments":{}}</tool_call>)",
      R"(<tool_call>{"name":"write","arguments":{"path":"pelican.svg"}}</tool_call>)",
      R"(<tool_call><function=write><parameter=path>pelican.svg</parameter></function></tool_call>)",
      R"(<tool_call>{"name":"write","arguments":{"path":"a","content":null}}</tool_call>)",
      R"(<tool_call>{"name":"write","arguments":{"path":"a","content":"SVG","count":1.5}}</tool_call>)"}) {
    std::size_t calls = 0;
    bool failed = false;
    try {
      Parser p(false, tools, [] { return "id"; }, {}, [&](const auto&, auto) { ++calls; });
      p.Feed(invalid);
      p.Finish();
    } catch (const std::runtime_error&) { failed = true; }
    Check(failed && calls == 0, "missing or mistyped arguments reached the tool client");
  }
  {
    std::string text;
    Parser p(false, tools, [] { return "id"; }, [&](auto s, bool) { text.append(s); });
    p.Feed("Visible before generation finishes.");
    Check(text == "Visible before generation finishes.", "tool request still buffers ordinary content");
  }
}
