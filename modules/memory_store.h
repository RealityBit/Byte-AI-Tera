#pragma once

#include <cstdint>
#include <string>
#include <vector>

// cross-session memory: a SQLite FTS5-backed log of every turn across every
// session, so Byte can recall things from earlier conversations, not just
// the current one. inspired by hermes-agent's FTS5 session search + LLM
// summarization for cross-session recall (https://github.com/NousResearch/hermes-agent)

struct memory_hit {
    int64_t     session_id;
    std::string started_at; // ISO 8601
    std::string role;       // "user" or "assistant"
    std::string content;
    std::string session_summary; // may be empty if the session has no summary yet
};

class memory_store {
public:
    explicit memory_store(const std::string & db_path);
    ~memory_store();

    // call once at startup; returns the new session's id
    int64_t start_session();

    void log_turn(int64_t session_id, const std::string & role, const std::string & content);

    // stores a one-shot summary of a finished session (call at exit)
    void set_session_summary(int64_t session_id, const std::string & summary);

    // full-text search across every past turn (all sessions), most relevant first
    std::vector<memory_hit> search(const std::string & query, int limit = 5);

    // deletes every past turn matching the query (across all sessions) and
    // returns how many rows were removed
    int forget(const std::string & query);

    // wipes every turn and session summary ever recorded; returns rows removed
    int forget_all();

private:
    struct sqlite3 * db = nullptr;
};
