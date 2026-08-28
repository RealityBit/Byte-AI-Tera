#pragma once

#include <string>

// port of Byte AI's time/date module, using the system's local timezone
// (whatever the OS is currently set to) rather than a hardcoded offset, so
// it stays correct automatically across DST changes and location moves

// true if the query is asking for the current time or date, including a
// follow-up naming another US timezone ("how about in pacific time?")
bool datetime_is_requested(const std::string & query);

// current date and time. if the query names another US timezone (Eastern,
// Central, Mountain, Pacific, Alaska, Hawaii, or UTC/GMT), the time is
// computed for that zone instead of the system's own, so conversions are an
// actual calculation, not the model guessing at the offset
std::string datetime_fetch(const std::string & query);
