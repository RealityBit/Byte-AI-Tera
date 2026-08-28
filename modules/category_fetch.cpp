#include "category_fetch.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <fstream>

using json = nlohmann::json;

namespace {

// raw.githubusercontent.com base for Byte's own curated knowledge base
const std::string BASE_URL = "https://raw.githubusercontent.com/RetroGigabyte/Byte-AI-Tera/main/data/knowledge/";

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

size_t curl_write_cb(char * ptr, size_t size, size_t nmemb, void * userdata) {
    auto * out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::optional<std::string> http_get(const std::string & url) {
    CURL * curl = curl_easy_init();
    if (!curl) {
        return std::nullopt;
    }

    std::string body;
    long status = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || status < 200 || status >= 300) {
        return std::nullopt;
    }
    return body;
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

category_fetch::category_fetch(std::string cache_path) : cache_path(std::move(cache_path)) {}

std::optional<std::string> category_fetch::best_matching_category(const std::string & query) {
    json cache = load_cache(cache_path);

    json manifest;
    if (cache.contains("__manifest")) {
        manifest = cache["__manifest"];
    } else {
        auto body = http_get(BASE_URL + "categories.json");
        if (!body) {
            return std::nullopt;
        }
        try {
            manifest = json::parse(*body);
        } catch (const std::exception &) {
            return std::nullopt;
        }
        cache["__manifest"] = manifest;
        save_cache(cache_path, cache);
    }

    if (!manifest.contains("categories")) {
        return std::nullopt;
    }

    std::string q = to_lower(query);
    std::string best_name;
    int best_score = 0;

    for (const auto & cat : manifest["categories"]) {
        int score = 0;
        for (const auto & kw : cat.value("keywords", json::array())) {
            std::string keyword = kw.get<std::string>();
            if (q.find(keyword) != std::string::npos) {
                score++;
            }
        }
        if (score > best_score) {
            best_score = score;
            best_name  = cat.value("name", "");
        }
    }

    if (best_name.empty()) {
        return std::nullopt;
    }
    return best_name;
}

bool category_fetch::is_requested(const std::string & query) {
    return best_matching_category(query).has_value();
}

std::optional<std::pair<std::string, std::string>> category_fetch::fetch(const std::string & query) {
    auto category = best_matching_category(query);
    if (!category) {
        return std::nullopt;
    }

    json cache = load_cache(cache_path);
    std::string cache_key = "chunk:" + *category;

    if (cache.contains(cache_key)) {
        return std::make_pair(*category, cache[cache_key].get<std::string>());
    }

    auto body = http_get(BASE_URL + *category + "/chunk-1.txt");
    if (!body) {
        return std::nullopt;
    }

    cache[cache_key] = *body;
    save_cache(cache_path, cache);

    return std::make_pair(*category, *body);
}
