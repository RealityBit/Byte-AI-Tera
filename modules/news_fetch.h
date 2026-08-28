#pragma once

#include <optional>
#include <string>

// port of Byte AI's news module (HackerNews + Dev.to feeds)
// see: https://github.com/RetroGigabyte/Byte_AI

// true if the query looks like a request for news ("hackernews", "dev.to", "news", ...)
bool news_is_requested(const std::string & query);

// fetches HackerNews top stories or Dev.to latest articles depending on the
// query text, as a formatted bullet list. "more" in the query asks for 20
// items instead of the default 10 (matching the original Byte AI behavior)
std::optional<std::string> news_fetch(const std::string & query);
