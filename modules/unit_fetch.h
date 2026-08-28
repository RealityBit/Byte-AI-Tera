#pragma once

#include <optional>
#include <string>

// unit converter: length, weight, volume, speed, and temperature, computed
// exactly in C++ (no API needed), matching patterns like "10km in miles",
// "100F to celsius", "5kg in lbs"

// true if the query looks like a "<value><unit> in/to <unit>" conversion
// between two units of the same category
bool unit_is_requested(const std::string & query);

// the computed conversion, formatted as "10 km = 6.2137 miles", or nullopt
// if the query didn't parse into two same-category units after all
std::optional<std::string> unit_fetch(const std::string & query);
