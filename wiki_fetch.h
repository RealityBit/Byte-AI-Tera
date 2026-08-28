#pragma once

#include <optional>
#include <string>
#include <vector>

// one turn of the conversation, used to build context-aware queries
struct wiki_turn {
    std::string user;
    std::string bot;
};

struct wiki_result {
    std::string title;
    std::vector<std::string> sentences;
    std::string learned_at; // ISO 8601 timestamp
};

// port of the Wiki-Fetch DAT module (https://github.com/RetroGigabyte/Wiki-Fetch)
// fetches Wikipedia summaries via the REST API, extracts sentences, and
// caches results to a JSON file on disk (the JS version uses localStorage)
class wiki_fetch {
public:
    explicit wiki_fetch(std::string cache_path);

    // fetch (or read from cache) the Wikipedia summary for a query, resolving
    // vague follow-ups ("tell me more") against the recent conversation history
    std::optional<wiki_result> learn(const std::string & query, const std::vector<wiki_turn> & history);

    // convert "X may refer to:" / "X includes:" style lines into a numbered list
    static std::string format_response(const std::string & text);

    // resolve a vague query into a concrete topic using conversation history
    std::string build_context_query(const std::string & query, const std::vector<wiki_turn> & history) const;

    // extract up to 5 keyword topics from the last 3 conversation turns
    static std::vector<std::string> get_conversation_context(const std::vector<wiki_turn> & history);

    size_t cache_size() const;
    void clear_cache_entry(const std::string & key);
    void clear_all_cache();

private:
    std::string cache_path;

    static std::string cache_key(const std::string & query);
    static std::vector<std::string> extract_sentences(const std::string & extract);
    std::optional<wiki_result> fetch_from_api(const std::string & title) const;
};
