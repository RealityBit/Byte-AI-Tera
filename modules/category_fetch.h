#pragma once

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

class category_fetch {
public:
    explicit category_fetch(std::string cache_path);

    // true if the query matches a known category's keywords
    bool is_requested(const std::string & query);

    // fetches (or reads from cache) the best-matching category's first chunk.
    // returns {category_name, content}, or nullopt if nothing matched
    std::optional<std::pair<std::string, std::string>> fetch(const std::string & query);

private:
    std::string cache_path;

    std::optional<std::string> best_matching_category(const std::string & query);
};
