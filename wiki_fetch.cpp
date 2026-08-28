#include "wiki_fetch.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace {

const std::set<std::string> stop_words = {
    "what", "when", "where", "which", "about", "that", "tell", "more"
};

size_t curl_write_cb(char * ptr, size_t size, size_t nmemb, void * userdata) {
    auto * out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
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

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

json load_cache(const std::string & path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return json::object();
    }
    try {
        json j;
        f >> j;
        return j;
    } catch (const std::exception &) {
        return json::object();
    }
}

void save_cache(const std::string & path, const json & cache) {
    std::ofstream f(path);
    if (f.is_open()) {
        f << cache.dump(2);
    }
}

} // namespace

wiki_fetch::wiki_fetch(std::string cache_path) : cache_path(std::move(cache_path)) {}

std::vector<std::string> wiki_fetch::get_conversation_context(const std::vector<wiki_turn> & history) {
    std::vector<std::string> context;
    if (history.empty()) {
        return context;
    }

    size_t start = history.size() > 3 ? history.size() - 3 : 0;
    for (size_t i = start; i < history.size(); i++) {
        if (history[i].user.empty()) {
            continue;
        }
        std::istringstream iss(to_lower(history[i].user));
        std::string word;
        while (iss >> word) {
            if (word.size() > 3 && stop_words.find(word) == stop_words.end()) {
                context.push_back(word);
            }
        }
    }

    if (context.size() > 5) {
        context.resize(5);
    }
    return context;
}

std::string wiki_fetch::build_context_query(const std::string & query, const std::vector<wiki_turn> & history) const {
    static const std::regex vague_re("^(tell|show|explain|more|details|about|that|it)");
    if (std::regex_search(to_lower(query), vague_re)) {
        auto context = get_conversation_context(history);
        if (!context.empty()) {
            return context[0];
        }
    }
    return query;
}

std::string wiki_fetch::cache_key(const std::string & query) {
    std::string key = to_lower(query);
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        if (c == '(' || c == ')') {
            continue;
        }
        out.push_back(std::isspace((unsigned char) c) ? '_' : c);
    }
    // collapse repeated underscores left by runs of whitespace
    out.erase(std::unique(out.begin(), out.end(), [](char a, char b) { return a == '_' && b == '_'; }), out.end());
    return out;
}

std::vector<std::string> wiki_fetch::extract_sentences(const std::string & extract) {
    std::vector<std::string> sentences;

    std::vector<std::string> paragraphs;
    size_t pos = 0;
    while (true) {
        size_t next = extract.find("\n\n", pos);
        paragraphs.push_back(extract.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
        if (next == std::string::npos) {
            break;
        }
        pos = next + 2;
    }

    static const std::regex sentence_re(R"([.!?]\s+)");

    for (const auto & para : paragraphs) {
        if (para.size() < 10) {
            continue;
        }

        std::vector<std::string> parts;
        std::sregex_token_iterator it(para.begin(), para.end(), sentence_re, -1);
        std::sregex_token_iterator end;
        for (; it != end; ++it) {
            parts.push_back(*it);
        }

        for (const auto & s : parts) {
            if (s.size() > 5 && s.size() < 500 && s.rfind("See also", 0) != 0 && s.rfind("References", 0) != 0) {
                sentences.push_back(s);
                if (sentences.size() >= 5) {
                    break;
                }
            }
        }
        if (sentences.size() >= 5) {
            break;
        }
    }

    if (sentences.empty() && extract.size() > 20) {
        sentences.push_back(extract.substr(0, 500));
    }

    return sentences;
}

std::optional<wiki_result> wiki_fetch::fetch_from_api(const std::string & title) const {
    CURL * curl = curl_easy_init();
    if (!curl) {
        return std::nullopt;
    }

    char * escaped = curl_easy_escape(curl, title.c_str(), (int) title.size());
    std::string url = "https://en.wikipedia.org/api/rest_v1/page/summary/" + std::string(escaped);
    curl_free(escaped);

    std::string body;
    long status = 0;

    struct curl_slist * headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: llama.cpp-wiki-chat/1.0 (https://github.com/ggml-org/llama.cpp)");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || status == 404) {
        return std::nullopt;
    }
    if (status < 200 || status >= 300) {
        return std::nullopt;
    }

    json data;
    try {
        data = json::parse(body);
    } catch (const std::exception &) {
        return std::nullopt;
    }

    if (!data.contains("extract") || data["extract"].get<std::string>().empty()) {
        return std::nullopt;
    }

    wiki_result result;
    result.title      = data.value("title", title);
    result.sentences  = extract_sentences(data["extract"].get<std::string>());
    result.learned_at = now_iso8601();

    if (result.sentences.empty()) {
        return std::nullopt;
    }

    return result;
}

std::optional<wiki_result> wiki_fetch::learn(const std::string & query, const std::vector<wiki_turn> & history) {
    const std::string context_query = build_context_query(query, history);
    const std::string key           = cache_key(context_query);

    json cache = load_cache(cache_path);
    if (cache.contains(key)) {
        const auto & entry = cache[key];
        wiki_result result;
        result.title      = entry.value("title", "");
        result.learned_at = entry.value("learned_at", "");
        result.sentences  = entry.value("sentences", std::vector<std::string>{});
        return result;
    }

    auto result = fetch_from_api(context_query);
    if (!result) {
        return std::nullopt;
    }

    cache[key] = {
        {"title", result->title},
        {"sentences", result->sentences},
        {"learned_at", result->learned_at},
    };
    save_cache(cache_path, cache);

    return result;
}

std::string wiki_fetch::format_response(const std::string & text) {
    std::vector<std::string> lines;
    {
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            // trim
            size_t a = line.find_first_not_of(" \t\r\n");
            size_t b = line.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) {
                continue;
            }
            lines.push_back(line.substr(a, b - a + 1));
        }
    }

    static const std::regex header_re(R"(may refer to:|can mean:|include[s]?:)", std::regex::icase);

    std::vector<std::string> result;
    bool in_list  = false;
    int  list_num = 1;

    for (const auto & line : lines) {
        if (std::regex_search(line, header_re)) {
            in_list  = true;
            list_num = 1;
            continue;
        }

        if (in_list) {
            size_t word_count = std::count(line.begin(), line.end(), ' ') + 1;
            if (line.size() > 150 && word_count > 20) {
                in_list = false;
                result.push_back(line);
            } else {
                result.push_back(std::to_string(list_num) + ". " + line);
                list_num++;
            }
            continue;
        }

        result.push_back(line);
    }

    std::string out;
    for (size_t i = 0; i < result.size(); i++) {
        if (i) {
            out += "\n";
        }
        out += result[i];
    }
    return out;
}

size_t wiki_fetch::cache_size() const {
    return load_cache(cache_path).size();
}

void wiki_fetch::clear_cache_entry(const std::string & key) {
    json cache = load_cache(cache_path);
    cache.erase(key);
    save_cache(cache_path, cache);
}

void wiki_fetch::clear_all_cache() {
    save_cache(cache_path, json::object());
}
