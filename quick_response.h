#pragma once

#include <optional>
#include <string>

// port of Byte AI 3.0's hardcoded quick replies for greetings, farewells, and
// acknowledgements -- answered instantly without invoking the model, same as
// math/datetime. identity questions ("who are you") are handled separately
// via the system prompt in wiki-chat.cpp, not here, since the model already
// answers those well.
std::optional<std::string> quick_response(const std::string & query);
