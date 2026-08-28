#include "datetime_fetch.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <map>
#include <sstream>
#include <vector>

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// friendly names for the US zones tzset()/%Z commonly report; anything else
// (e.g. "GMT", "CET") is left as-is since it already reads fine on its own
std::string friendly_zone_name(const std::string & abbrev) {
    static const std::map<std::string, std::string> names = {
        {"EST", "Eastern time"}, {"EDT", "Eastern time"},
        {"CST", "Central time"}, {"CDT", "Central time"},
        {"MST", "Mountain time"}, {"MDT", "Mountain time"},
        {"PST", "Pacific time"}, {"PDT", "Pacific time"},
        {"AKST", "Alaska time"}, {"AKDT", "Alaska time"},
        {"HST", "Hawaii time"}, {"HDT", "Hawaii time"},
    };
    auto it = names.find(abbrev);
    return it != names.end() ? it->second : abbrev;
}

// standard-time UTC offsets (hours) for the named US zones; the daylight
// variant is one hour ahead of these, except Hawaii which does not observe DST
struct zone_info {
    std::string keyword;
    std::string label;
    int         utc_offset_std;
    bool        observes_dst;
};

const std::vector<zone_info> & known_zones() {
    static const std::vector<zone_info> zones = {
        {"eastern", "Eastern time", -5, true},
        {"central", "Central time", -6, true},
        {"mountain", "Mountain time", -7, true},
        {"pacific", "Pacific time", -8, true},
        {"alaska", "Alaska time", -9, true},
        {"hawaii", "Hawaii time", -10, false},
        {"utc", "UTC", 0, false},
        {"gmt", "GMT", 0, false},
    };
    return zones;
}

int levenshtein(const std::string & a, const std::string & b) {
    std::vector<std::vector<int>> d(a.size() + 1, std::vector<int>(b.size() + 1));
    for (size_t i = 0; i <= a.size(); i++) d[i][0] = (int) i;
    for (size_t j = 0; j <= b.size(); j++) d[0][j] = (int) j;
    for (size_t i = 1; i <= a.size(); i++) {
        for (size_t j = 1; j <= b.size(); j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
        }
    }
    return d[a.size()][b.size()];
}

// finds a known zone by exact substring match, falling back to a fuzzy match
// (edit distance <= 2) against each word so typos like "paffic" still resolve
// to "pacific" instead of falling through to an unrelated Wikipedia lookup
const zone_info * find_zone(const std::string & q) {
    for (const auto & z : known_zones()) {
        if (q.find(z.keyword) != std::string::npos) {
            return &z;
        }
    }

    std::istringstream iss(q);
    std::string word;
    while (iss >> word) {
        if (word.size() < 4) {
            continue;
        }
        for (const auto & z : known_zones()) {
            if (z.keyword.size() < 4) {
                continue; // "utc"/"gmt" are too short to fuzzy-match safely
            }
            if (levenshtein(word, z.keyword) <= 2) {
                return &z;
            }
        }
    }
    return nullptr;
}

} // namespace

bool datetime_is_requested(const std::string & query) {
    std::string q = to_lower(query);
    if (q.find("what time") != std::string::npos ||
        q.find("current time") != std::string::npos ||
        q.find("what day") != std::string::npos ||
        q.find("what date") != std::string::npos ||
        q.find("today's date") != std::string::npos ||
        q.find("todays date") != std::string::npos) {
        return true;
    }

    // a bare "<zone> time" follow-up, e.g. "how about in pacific time?"
    // (or a typo of one, e.g. "how about paffic time?")
    return q.find("time") != std::string::npos && find_zone(q) != nullptr;
}

std::string datetime_fetch(const std::string & query) {
    // tzset() picks up the OS timezone (and any DST rule change) on every call,
    // so localtime_r below always reflects the system's current zone
    tzset();

    std::time_t now = std::time(nullptr);
    std::tm local_tm;
    localtime_r(&now, &local_tm);

    std::string q = to_lower(query);
    const zone_info * requested = find_zone(q);

    std::tm tm;
    std::string zone_label;

    if (requested) {
        // system's DST status tells us whether US zones are currently observing
        // daylight time; apply the same rule to the requested zone's offset
        int offset_hours = requested->utc_offset_std + ((local_tm.tm_isdst > 0 && requested->observes_dst) ? 1 : 0);

        std::time_t target = now + offset_hours * 3600;
        gmtime_r(&target, &tm);
        zone_label = requested->label;
    } else {
        tm = local_tm;
        char zone_buf[16];
        std::strftime(zone_buf, sizeof(zone_buf), "%Z", &local_tm);
        zone_label = friendly_zone_name(zone_buf);
    }

    char date_buf[64];
    std::strftime(date_buf, sizeof(date_buf), "%A %B %-d %Y", &tm);

    char time_buf[16];
    std::strftime(time_buf, sizeof(time_buf), "%-I:%M %p", &tm);

    return "Today is " + std::string(date_buf) + " at " + time_buf + " " + zone_label;
}
