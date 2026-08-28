#include "news_fetch.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <ctime>

using json = nlohmann::json;

namespace {

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

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string format_datetime(time_t unix_time) {
    std::tm tm;
    localtime_r(&unix_time, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%-m/%-d/%Y, %-I:%M %p", &tm);
    return buf;
}

std::optional<std::string> fetch_hackernews(int limit) {
    auto ids_body = http_get("https://hacker-news.firebaseio.com/v0/topstories.json");
    if (!ids_body) {
        return std::nullopt;
    }

    json ids;
    try {
        ids = json::parse(*ids_body);
    } catch (const std::exception &) {
        return std::nullopt;
    }

    std::string text = "HackerNews Top Stories\n\n";
    int count = 0;

    for (size_t i = 0; i < ids.size() && count < limit; i++) {
        auto story_body = http_get("https://hacker-news.firebaseio.com/v0/item/" + ids[i].dump() + ".json");
        if (!story_body) {
            continue;
        }

        json story;
        try {
            story = json::parse(*story_body);
        } catch (const std::exception &) {
            continue;
        }

        if (!story.contains("title") || story["title"].get<std::string>().empty()) {
            continue;
        }

        int points = story.value("score", 0);
        time_t time  = story.value("time", 0);
        text += "- " + story["title"].get<std::string>() + "\n  " + std::to_string(points) +
                " pts | " + format_datetime(time) + "\n\n";
        count++;
    }

    return text;
}

std::optional<std::string> fetch_devto(int limit) {
    auto body = http_get("https://dev.to/api/articles?per_page=" + std::to_string(limit) + "&sort_by=latest");
    if (!body) {
        return std::nullopt;
    }

    json articles;
    try {
        articles = json::parse(*body);
    } catch (const std::exception &) {
        return std::nullopt;
    }

    if (!articles.is_array()) {
        return std::nullopt;
    }

    std::string text = "Dev.to Latest Articles\n\n";
    for (const auto & article : articles) {
        int reactions = article.value("public_reactions_count", 0);
        text += "- " + article.value("title", "") + "\n  " + std::to_string(reactions) + " reactions\n\n";
    }

    return text;
}

} // namespace

bool news_is_requested(const std::string & query) {
    std::string q = to_lower(query);
    return q.find("hackernews") != std::string::npos ||
           q.find("hacker news") != std::string::npos ||
           q.find("dev.to") != std::string::npos ||
           q.find("devto") != std::string::npos ||
           q.find("news") != std::string::npos;
}

std::optional<std::string> news_fetch(const std::string & query) {
    std::string q = to_lower(query);
    int limit = q.find("more") != std::string::npos ? 20 : 10;

    if (q.find("dev.to") != std::string::npos || q.find("devto") != std::string::npos) {
        return fetch_devto(limit);
    }
    return fetch_hackernews(limit);
}
