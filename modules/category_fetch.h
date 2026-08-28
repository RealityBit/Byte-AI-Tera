#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

// dynamically-loaded knowledge base distilled from a bigger model (Qwen2.5
// 3B), split into GitHub-sized chunks and fetched on demand -- the same
// pattern as wiki_fetch, but sourced from Byte's own curated data
// (data/knowledge/ in the Byte-AI-Tera repo) instead of Wikipedia.
//
// category matching is driven by a manifest (categories.json) fetched from
// GitHub and cached locally, so adding a new category is just adding data +
// a manifest entry -- no code change needed.
//
// the local on-disk cache grows dynamically as you ask about more topics,
// bounded by a configurable byte budget (e.g. smaller on a phone, larger on
// a MacBook) -- once the budget is exceeded, the least-recently-used chunks
// are evicted first to make room for newly fetched ones.

class category_fetch {
public:
    // budget_bytes caps the local cache's total size; 0 means unbounded
    category_fetch(std::string cache_path, uint64_t budget_bytes = 0);

    // true if the query matches a known category's keywords
    bool is_requested(const std::string & query);

    // fetches (or reads from cache) the best-matching category's first chunk.
    // returns {category_name, content}, or nullopt if nothing matched
    std::optional<std::pair<std::string, std::string>> fetch(const std::string & query);

    // total bytes currently held in the local cache (chunk content only,
    // not the manifest)
    uint64_t cache_size_bytes();

private:
    std::string cache_path;
    uint64_t    budget_bytes;

    std::optional<std::string> best_matching_category(const std::string & query);
};
