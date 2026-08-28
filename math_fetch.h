#pragma once

#include <optional>
#include <string>

// port of Byte AI's math module: direct expressions ("2+3") and simple
// word-based ones ("what is 5 plus 3") are computed exactly in C++, rather
// than left to the model to guess at arithmetic

// true if the query contains a two-operand expression this module can evaluate
bool math_is_requested(const std::string & query);

// the computed result, formatted as "a op b = result", or nullopt if the
// query didn't parse into a two-operand expression after all
std::optional<std::string> math_fetch(const std::string & query);
