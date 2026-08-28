#pragma once

#include <optional>
#include <string>
#include <vector>

// local-only scheduled automations: installs a daily cron job (via the
// user's own crontab) that re-invokes llama-wiki-chat in --batch mode at a
// given time, appending its reply to a local report file. No external
// delivery integrations -- inspired by hermes-agent's built-in cron
// scheduler (https://github.com/NousResearch/hermes-agent), scoped down to
// local file output only.

struct scheduled_job {
    std::string name;   // unique tag, e.g. "byte-1"
    std::string time;   // "HH:MM", 24-hour
    std::string prompt;
};

// installs a daily cron entry running the given binary in --batch mode at
// time_hhmm ("HH:MM"), appending its reply to report_path. Returns the new
// job's name on success.
std::optional<std::string> schedule_add(const std::string & time_hhmm, const std::string & prompt,
                                         const std::string & binary_path, const std::string & model_path,
                                         int ngl, const std::string & report_path);

// lists every Byte-installed cron job (this user's crontab only)
std::vector<scheduled_job> schedule_list();

// removes a job by name; returns false if no such job was found
bool schedule_remove(const std::string & name);
