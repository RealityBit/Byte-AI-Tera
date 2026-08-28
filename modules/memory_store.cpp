#include "memory_store.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace {

// builds a safe FTS5 MATCH expression from free-form user text: each word is
// quoted as a literal token (doubling any embedded quotes) and OR'd together,
// so punctuation/operators in the query can't break FTS5's query syntax
std::string sanitize_fts_query(const std::string & query) {
    std::istringstream iss(query);
    std::string word, out;
    bool first = true;

    while (iss >> word) {
        std::string escaped;
        for (char c : word) {
            if (c == '"') {
                escaped += "\"\"";
            } else {
                escaped += c;
            }
        }
        if (escaped.empty()) {
            continue;
        }
        if (!first) {
            out += " OR ";
        }
        out += "\"" + escaped + "\"";
        first = false;
    }
    return out;
}

std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void exec_or_die(sqlite3 * db, const char * sql) {
    char * err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        fprintf(stderr, "memory_store: %s\n", err);
        sqlite3_free(err);
    }
}

} // namespace

memory_store::memory_store(const std::string & db_path) {
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        fprintf(stderr, "memory_store: failed to open %s\n", db_path.c_str());
        db = nullptr;
        return;
    }

    exec_or_die(db, "CREATE TABLE IF NOT EXISTS sessions ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                     "started_at TEXT NOT NULL, "
                     "summary TEXT DEFAULT '');");

    exec_or_die(db, "CREATE TABLE IF NOT EXISTS turns ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                     "session_id INTEGER NOT NULL, "
                     "role TEXT NOT NULL, "
                     "content TEXT NOT NULL);");

    exec_or_die(db, "CREATE VIRTUAL TABLE IF NOT EXISTS turns_fts USING fts5("
                     "content, content='turns', content_rowid='id');");

    // keep the FTS index in sync with the turns table automatically
    exec_or_die(db, "CREATE TRIGGER IF NOT EXISTS turns_ai AFTER INSERT ON turns BEGIN "
                     "INSERT INTO turns_fts(rowid, content) VALUES (new.id, new.content); END;");

    // external-content fts5 tables need the special 'delete' command form to
    // stay in sync when a row is removed from the backing turns table
    exec_or_die(db, "CREATE TRIGGER IF NOT EXISTS turns_ad AFTER DELETE ON turns BEGIN "
                     "INSERT INTO turns_fts(turns_fts, rowid, content) VALUES ('delete', old.id, old.content); END;");
}

memory_store::~memory_store() {
    if (db) {
        sqlite3_close(db);
    }
}

int64_t memory_store::start_session() {
    if (!db) {
        return -1;
    }

    sqlite3_stmt * stmt = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO sessions (started_at) VALUES (?);", -1, &stmt, nullptr);
    std::string ts = now_iso8601();
    sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return sqlite3_last_insert_rowid(db);
}

void memory_store::log_turn(int64_t session_id, const std::string & role, const std::string & content) {
    if (!db || session_id < 0) {
        return;
    }

    sqlite3_stmt * stmt = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO turns (session_id, role, content) VALUES (?, ?, ?);", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void memory_store::set_session_summary(int64_t session_id, const std::string & summary) {
    if (!db || session_id < 0) {
        return;
    }

    sqlite3_stmt * stmt = nullptr;
    sqlite3_prepare_v2(db, "UPDATE sessions SET summary = ? WHERE id = ?;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, summary.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<memory_hit> memory_store::search(const std::string & query, int limit) {
    std::vector<memory_hit> hits;
    if (!db) {
        return hits;
    }

    static const char * sql =
        "SELECT s.id, s.started_at, t.role, t.content, s.summary "
        "FROM turns_fts "
        "JOIN turns t ON t.id = turns_fts.rowid "
        "JOIN sessions s ON s.id = t.session_id "
        "WHERE turns_fts MATCH ? "
        "ORDER BY rank LIMIT ?;";

    sqlite3_stmt * stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return hits;
    }

    std::string fts_query = sanitize_fts_query(query);
    sqlite3_bind_text(stmt, 1, fts_query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        memory_hit hit;
        hit.session_id      = sqlite3_column_int64(stmt, 0);
        hit.started_at      = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        hit.role            = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        hit.content         = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        const unsigned char * summary = sqlite3_column_text(stmt, 4);
        hit.session_summary = summary ? reinterpret_cast<const char *>(summary) : "";
        hits.push_back(std::move(hit));
    }

    sqlite3_finalize(stmt);
    return hits;
}

int memory_store::forget(const std::string & query) {
    if (!db) {
        return 0;
    }

    static const char * sql =
        "DELETE FROM turns WHERE id IN (SELECT rowid FROM turns_fts WHERE turns_fts MATCH ?);";

    sqlite3_stmt * stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    std::string fts_query = sanitize_fts_query(query);
    sqlite3_bind_text(stmt, 1, fts_query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return sqlite3_changes(db);
}

int memory_store::forget_all() {
    if (!db) {
        return 0;
    }

    exec_or_die(db, "DELETE FROM turns;");
    int removed = sqlite3_changes(db);
    exec_or_die(db, "UPDATE sessions SET summary = '';");

    return removed;
}
