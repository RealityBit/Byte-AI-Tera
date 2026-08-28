#include "math_fetch.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <regex>
#include <sstream>
#include <vector>

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

const std::map<std::string, char> & word_ops() {
    static const std::map<std::string, char> ops = {
        {"plus", '+'}, {"added to", '+'},
        {"minus", '-'}, {"subtracted from", '-'},
        {"times", '*'}, {"multiplied by", '*'},
        {"divided by", '/'}, {"over", '/'},
    };
    return ops;
}

// matches a plain symbolic expression: "2+3", "10 * 5.5"
const std::regex & symbolic_re() {
    static const std::regex re(R"((-?\d+(?:\.\d+)?)\s*([+\-*/^])\s*(-?\d+(?:\.\d+)?))");
    return re;
}

std::string format_number(double v) {
    if (v == std::floor(v) && std::abs(v) < 1e15) {
        std::ostringstream oss;
        oss << (long long) v;
        return oss.str();
    }
    std::ostringstream oss;
    oss.precision(6);
    oss << v;
    return oss.str();
}

std::optional<double> apply_op(double a, char op, double b) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b != 0 ? std::optional<double>(a / b) : std::nullopt;
        case '^': return std::pow(a, b);
        default:  return std::nullopt;
    }
}

} // namespace

bool math_is_requested(const std::string & query) {
    if (std::regex_search(query, symbolic_re())) {
        return true;
    }

    std::string q = to_lower(query);
    static const std::regex number_re(R"(-?\d+(?:\.\d+)?)");
    auto begin = std::sregex_iterator(q.begin(), q.end(), number_re);
    if (std::distance(begin, std::sregex_iterator()) < 2) {
        return false;
    }
    for (const auto & [word, op] : word_ops()) {
        if (q.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> math_fetch(const std::string & query) {
    std::smatch m;
    if (std::regex_search(query, m, symbolic_re())) {
        double a  = std::stod(m[1]);
        char   op = m[2].str()[0];
        double b  = std::stod(m[3]);
        auto result = apply_op(a, op, b);
        if (!result) {
            return std::nullopt;
        }
        return m[1].str() + " " + op + " " + m[3].str() + " = " + format_number(*result);
    }

    std::string q = to_lower(query);
    static const std::regex number_re(R"(-?\d+(?:\.\d+)?)");
    std::vector<double> numbers;
    for (auto it = std::sregex_iterator(q.begin(), q.end(), number_re); it != std::sregex_iterator(); ++it) {
        numbers.push_back(std::stod(it->str()));
    }
    if (numbers.size() < 2) {
        return std::nullopt;
    }

    for (const auto & [word, op] : word_ops()) {
        if (q.find(word) != std::string::npos) {
            auto result = apply_op(numbers[0], op, numbers[1]);
            if (!result) {
                return std::nullopt;
            }
            return format_number(numbers[0]) + " " + op + " " + format_number(numbers[1]) +
                   " = " + format_number(*result);
        }
    }
    return std::nullopt;
}
