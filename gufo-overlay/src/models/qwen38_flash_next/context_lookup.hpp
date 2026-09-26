#ifndef GUFO_MODELS_QWEN38_FLASH_NEXT_CONTEXT_LOOKUP_HPP_
#define GUFO_MODELS_QWEN38_FLASH_NEXT_CONTEXT_LOOKUP_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace gufo::models::qwen38_flash_next {

struct ContextLookupMatch {
  std::vector<std::int32_t> continuation;
  std::uint32_t ngram{0};
  std::uint32_t source_end{0};
  std::uint32_t distance{0};
};

/// Finds a continuation already present in the committed context. The query is
/// the longest suffix ending in `anchor`; matches are searched newest-first so
/// local code/JSON/tool patterns win over stale distant text. This is a pure
/// proposal source: target verification remains authoritative.
inline std::optional<ContextLookupMatch> FindContextLookup(
    std::span<const std::int32_t> history, std::int32_t anchor,
    std::uint32_t max_tokens, std::uint32_t min_ngram,
    std::uint32_t max_ngram, std::uint32_t search_window,
    std::uint32_t min_draft_tokens) {
  if (max_tokens == 0 || min_draft_tokens == 0 || history.empty())
    return std::nullopt;
  min_ngram = std::max<std::uint32_t>(2, min_ngram);
  max_ngram = std::max(min_ngram, max_ngram);
  const std::uint32_t available_order = static_cast<std::uint32_t>(
      std::min<std::size_t>(history.size() + 1, max_ngram));
  if (available_order < min_ngram) return std::nullopt;

  const std::size_t history_size = history.size();
  const std::size_t window = search_window == 0
                                 ? history_size
                                 : std::min<std::size_t>(search_window,
                                                         history_size);
  const std::size_t search_floor = history_size - window;

  for (std::uint32_t order = available_order;; --order) {
    const std::size_t prefix = order - 1;
    if (history_size >= prefix + 1) {
      // candidate_end must leave at least one historical token after the
      // matched n-gram to use as a continuation.
      std::size_t candidate_end = history_size - 2;
      const std::size_t earliest = std::max(search_floor + prefix, prefix);
      if (candidate_end >= earliest) {
        for (;;) {
          if (history[candidate_end] == anchor) {
            bool same = true;
            const std::size_t query_begin = history_size - prefix;
            const std::size_t candidate_begin = candidate_end - prefix;
            for (std::size_t i = 0; i < prefix; ++i) {
              if (history[query_begin + i] != history[candidate_begin + i]) {
                same = false;
                break;
              }
            }
            if (same) {
              const std::size_t continuation_begin = candidate_end + 1;
              const std::size_t continuation_count = std::min<std::size_t>(
                  max_tokens, history_size - continuation_begin);
              if (continuation_count >= min_draft_tokens) {
                ContextLookupMatch match;
                match.ngram = order;
                match.source_end = static_cast<std::uint32_t>(candidate_end);
                match.distance = static_cast<std::uint32_t>(
                    history_size - 1 - candidate_end);
                match.continuation.assign(
                    history.begin() + continuation_begin,
                    history.begin() + continuation_begin + continuation_count);
                return match;
              }
            }
          }
          if (candidate_end == earliest) break;
          --candidate_end;
        }
      }
    }
    if (order == min_ngram) break;
  }
  return std::nullopt;
}

}  // namespace gufo::models::qwen38_flash_next

#endif
