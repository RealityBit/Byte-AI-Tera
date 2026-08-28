#pragma once

#include <optional>
#include <string>

// port of Byte AI 3.0's weather module (wttr.in), which 3.0 itself listed as
// "in development" due to API issues -- ported as-is here since wttr.in's
// json format has since stabilized

// true if the query is asking about the weather
bool weather_is_requested(const std::string & query);

// current conditions for the location named in the query ("weather in Tokyo"),
// or the caller's own location via IP geolocation if none is named
std::optional<std::string> weather_fetch(const std::string & query);
