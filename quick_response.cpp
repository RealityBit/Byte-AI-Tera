#include "quick_response.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

const std::map<std::string, std::string> & responses() {
    static const std::map<std::string, std::string> map = {
        {"hello", "Hey there! What can I help with?"},
        {"hi", "Hi! What's up?"},
        {"hey", "Hey! What's on your mind?"},
        {"greetings", "Greetings! Ask me anything."},
        {"good morning", "Good morning! What can I help with?"},
        {"good afternoon", "Good afternoon! What do you need?"},
        {"good evening", "Good evening! How can I help?"},
        {"good night", "Good night!"},
        {"goodbye", "Goodbye! Come back anytime."},
        {"bye", "See you later!"},
        {"see you", "See you later!"},
        {"farewell", "Farewell!"},
        {"help", "I'm Byte 4.0 \"Tera\". Ask me anything -- I can look things up on Wikipedia, "
                  "check HackerNews/Dev.to, do math, tell you the time in any US timezone, and "
                  "check the weather. Try \"tell me more\" after a question for follow-ups."},
        {"how are you", "Doing well, thanks! How can I help?"},
        {"thanks", "You're welcome!"},
        {"thank you", "You're welcome!"},
        {"ok", "Sounds good -- anything else?"},
        {"okay", "Sounds good -- anything else?"},
        {"sure", "Great, what would you like to know?"},
    };
    return map;
}

} // namespace

std::optional<std::string> quick_response(const std::string & query, const std::string & user_name) {
    std::string lower = to_lower(query);

    static const std::regex trailing_punct(R"([.!?;,]+$)");
    std::string clean = std::regex_replace(lower, trailing_punct, "");

    static const std::regex who_am_i_re(R"(\bwho am i\b)");
    if (std::regex_search(clean, who_am_i_re)) {
        return user_name.empty() ? "You're the one asking the questions! What would you like to know?"
                                  : "You're " + user_name + "!";
    }

    auto & map = responses();

    if (auto it = map.find(clean); it != map.end()) {
        return it->second;
    }

    for (const auto & [key, reply] : map) {
        bool multi_word = key.find(' ') != std::string::npos;
        if (multi_word) {
            if (clean.find(key) != std::string::npos) {
                return reply;
            }
        } else {
            std::regex word_boundary("\\b" + key + "\\b");
            if (std::regex_search(clean, word_boundary)) {
                return reply;
            }
        }
    }

    return std::nullopt;
}
