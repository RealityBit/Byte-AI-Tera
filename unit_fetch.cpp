#include "unit_fetch.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

struct unit_info {
    std::string category;    // "length", "weight", "volume", "speed", "temp"
    double      factor;      // to the category's base unit; unused for temp
    char        temp_symbol; // 'C'/'F'/'K' for temp units, 0 otherwise
    std::string label;       // canonical display name
};

const std::unordered_map<std::string, unit_info> & aliases() {
    static const std::unordered_map<std::string, unit_info> map = {
        // length, base = meters
        {"km", {"length", 1000.0, 0, "km"}}, {"kilometer", {"length", 1000.0, 0, "km"}}, {"kilometers", {"length", 1000.0, 0, "km"}},
        {"m", {"length", 1.0, 0, "m"}}, {"meter", {"length", 1.0, 0, "m"}}, {"meters", {"length", 1.0, 0, "m"}},
        {"cm", {"length", 0.01, 0, "cm"}}, {"centimeter", {"length", 0.01, 0, "cm"}}, {"centimeters", {"length", 0.01, 0, "cm"}},
        {"mm", {"length", 0.001, 0, "mm"}}, {"millimeter", {"length", 0.001, 0, "mm"}}, {"millimeters", {"length", 0.001, 0, "mm"}},
        {"mile", {"length", 1609.344, 0, "miles"}}, {"miles", {"length", 1609.344, 0, "miles"}}, {"mi", {"length", 1609.344, 0, "miles"}},
        {"yard", {"length", 0.9144, 0, "yards"}}, {"yards", {"length", 0.9144, 0, "yards"}}, {"yd", {"length", 0.9144, 0, "yards"}},
        {"foot", {"length", 0.3048, 0, "feet"}}, {"feet", {"length", 0.3048, 0, "feet"}}, {"ft", {"length", 0.3048, 0, "feet"}},
        {"inch", {"length", 0.0254, 0, "inches"}}, {"inches", {"length", 0.0254, 0, "inches"}},

        // weight, base = kg
        {"kg", {"weight", 1.0, 0, "kg"}}, {"kilogram", {"weight", 1.0, 0, "kg"}}, {"kilograms", {"weight", 1.0, 0, "kg"}},
        {"g", {"weight", 0.001, 0, "g"}}, {"gram", {"weight", 0.001, 0, "g"}}, {"grams", {"weight", 0.001, 0, "g"}},
        {"lb", {"weight", 0.453592, 0, "lbs"}}, {"lbs", {"weight", 0.453592, 0, "lbs"}},
        {"pound", {"weight", 0.453592, 0, "lbs"}}, {"pounds", {"weight", 0.453592, 0, "lbs"}},
        {"oz", {"weight", 0.0283495, 0, "oz"}}, {"ounce", {"weight", 0.0283495, 0, "oz"}}, {"ounces", {"weight", 0.0283495, 0, "oz"}},
        {"ton", {"weight", 1000.0, 0, "tons"}}, {"tons", {"weight", 1000.0, 0, "tons"}},
        {"tonne", {"weight", 1000.0, 0, "tons"}}, {"tonnes", {"weight", 1000.0, 0, "tons"}},

        // volume, base = liters
        {"l", {"volume", 1.0, 0, "liters"}}, {"liter", {"volume", 1.0, 0, "liters"}}, {"liters", {"volume", 1.0, 0, "liters"}},
        {"litre", {"volume", 1.0, 0, "liters"}}, {"litres", {"volume", 1.0, 0, "liters"}},
        {"ml", {"volume", 0.001, 0, "ml"}}, {"milliliter", {"volume", 0.001, 0, "ml"}}, {"milliliters", {"volume", 0.001, 0, "ml"}},
        {"gallon", {"volume", 3.78541, 0, "gallons"}}, {"gallons", {"volume", 3.78541, 0, "gallons"}}, {"gal", {"volume", 3.78541, 0, "gallons"}},
        {"quart", {"volume", 0.946353, 0, "quarts"}}, {"quarts", {"volume", 0.946353, 0, "quarts"}}, {"qt", {"volume", 0.946353, 0, "quarts"}},
        {"pint", {"volume", 0.473176, 0, "pints"}}, {"pints", {"volume", 0.473176, 0, "pints"}}, {"pt", {"volume", 0.473176, 0, "pints"}},
        {"cup", {"volume", 0.236588, 0, "cups"}}, {"cups", {"volume", 0.236588, 0, "cups"}},

        // speed, base = km/h
        {"kmh", {"speed", 1.0, 0, "km/h"}}, {"kph", {"speed", 1.0, 0, "km/h"}}, {"km/h", {"speed", 1.0, 0, "km/h"}},
        {"mph", {"speed", 1.60934, 0, "mph"}},
        {"knot", {"speed", 1.852, 0, "knots"}}, {"knots", {"speed", 1.852, 0, "knots"}},

        // temperature (bridged via celsius, see convert_temp)
        {"celsius", {"temp", 0.0, 'C', "Celsius"}}, {"c", {"temp", 0.0, 'C', "Celsius"}},
        {"fahrenheit", {"temp", 0.0, 'F', "Fahrenheit"}}, {"f", {"temp", 0.0, 'F', "Fahrenheit"}},
        {"kelvin", {"temp", 0.0, 'K', "Kelvin"}}, {"k", {"temp", 0.0, 'K', "Kelvin"}},
    };
    return map;
}

const unit_info * find_unit(const std::string & word) {
    auto & map = aliases();
    auto   it  = map.find(to_lower(word));
    return it != map.end() ? &it->second : nullptr;
}

// bridges any temp unit through celsius
double to_celsius(double v, char symbol) {
    switch (symbol) {
        case 'F': return (v - 32.0) * 5.0 / 9.0;
        case 'K': return v - 273.15;
        default:  return v;
    }
}

double from_celsius(double c, char symbol) {
    switch (symbol) {
        case 'F': return c * 9.0 / 5.0 + 32.0;
        case 'K': return c + 273.15;
        default:  return c;
    }
}

std::string format_number(double v) {
    if (std::abs(v - std::round(v)) < 1e-9) {
        std::ostringstream oss;
        oss << (long long) std::round(v);
        return oss.str();
    }
    std::ostringstream oss;
    oss.precision(4);
    oss << v;
    return oss.str();
}

// matches "<value><unit> in/to <unit>", e.g. "10km in miles", "100F to celsius"
const std::regex & conversion_re() {
    static const std::regex re(R"((-?\d+(?:\.\d+)?)\s*([A-Za-z/]+)\s+(?:in|to)\s+([A-Za-z/]+))");
    return re;
}

struct parsed_conversion {
    double            value;
    const unit_info * from;
    const unit_info * to;
};

std::optional<parsed_conversion> parse(const std::string & query) {
    std::smatch m;
    if (!std::regex_search(query, m, conversion_re())) {
        return std::nullopt;
    }

    const unit_info * from = find_unit(m[2].str());
    const unit_info * to   = find_unit(m[3].str());
    if (!from || !to || from->category != to->category) {
        return std::nullopt;
    }

    return parsed_conversion{std::stod(m[1]), from, to};
}

} // namespace

bool unit_is_requested(const std::string & query) {
    return parse(query).has_value();
}

std::optional<std::string> unit_fetch(const std::string & query) {
    auto p = parse(query);
    if (!p) {
        return std::nullopt;
    }

    double result;
    if (p->from->category == "temp") {
        result = from_celsius(to_celsius(p->value, p->from->temp_symbol), p->to->temp_symbol);
    } else {
        result = p->value * p->from->factor / p->to->factor;
    }

    return format_number(p->value) + " " + p->from->label + " = " + format_number(result) + " " + p->to->label;
}
