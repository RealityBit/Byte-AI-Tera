#include "weather_fetch.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <regex>

using json = nlohmann::json;

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

size_t curl_write_cb(char * ptr, size_t size, size_t nmemb, void * userdata) {
    auto * out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// pulls a location out of "weather in <place>" / "weather for <place>"; falls
// back to empty, which wttr.in resolves via the requester's IP geolocation
std::string extract_location(const std::string & query) {
    static const std::regex loc_re(R"(weather (?:in|for|at)\s+(.+?)[\?\.!]*$)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(query, m, loc_re)) {
        return m[1].str();
    }
    return "";
}

} // namespace

bool weather_is_requested(const std::string & query) {
    std::string q = to_lower(query);
    return q.find("weather") != std::string::npos;
}

std::optional<std::string> weather_fetch(const std::string & query) {
    std::string location = extract_location(query);

    CURL * curl = curl_easy_init();
    if (!curl) {
        return std::nullopt;
    }

    char * escaped = curl_easy_escape(curl, location.c_str(), (int) location.size());
    std::string url = "https://wttr.in/" + std::string(escaped) + "?format=j1";
    curl_free(escaped);

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

    json data;
    try {
        data = json::parse(body);
    } catch (const std::exception &) {
        return std::nullopt;
    }

    if (!data.contains("current_condition") || data["current_condition"].empty()) {
        return std::nullopt;
    }

    const auto & current = data["current_condition"][0];
    std::string temp_c     = current.value("temp_C", "?");
    std::string humidity   = current.value("humidity", "?");
    std::string wind_kph   = current.value("windspeedKmph", "?");
    std::string condition  = "unknown";
    if (current.contains("weatherDesc") && !current["weatherDesc"].empty()) {
        condition = current["weatherDesc"][0].value("value", "unknown");
    }

    std::string area_name = "your location";
    if (data.contains("nearest_area") && !data["nearest_area"].empty()) {
        const auto & area = data["nearest_area"][0];
        if (area.contains("areaName") && !area["areaName"].empty()) {
            area_name = area["areaName"][0].value("value", area_name);
        }
    }

    return "Weather in " + area_name + ": " + temp_c + " C, " + condition +
           ", humidity " + humidity + "%, wind " + wind_kph + " km/h";
}
