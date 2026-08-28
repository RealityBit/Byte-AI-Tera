#include "scheduler.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <regex>
#include <sstream>

namespace {

const std::string MARKER = "# byte-ai:";

// wraps a value in single quotes for safe use as one shell argument,
// escaping any embedded single quotes (the standard 'foo'\''bar' trick)
std::string shell_quote(const std::string & s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string run_capture(const std::string & cmd) {
    std::string out;
    FILE * pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return out;
    }
    std::array<char, 4096> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        out.append(buf.data(), n);
    }
    pclose(pipe);
    return out;
}

bool write_crontab(const std::string & content) {
    FILE * pipe = popen("crontab -", "w");
    if (!pipe) {
        return false;
    }
    fwrite(content.data(), 1, content.size(), pipe);
    return pclose(pipe) == 0;
}

std::vector<std::string> current_crontab_lines() {
    std::string raw = run_capture("crontab -l 2>/dev/null");
    std::vector<std::string> lines;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

// extracts the single-quoted shell argument that immediately follows `flag`
// in a cron command line, reversing shell_quote's escaping
std::string extract_quoted_arg(const std::string & line, const std::string & flag) {
    size_t pos = line.find(flag);
    if (pos == std::string::npos) {
        return "";
    }
    pos = line.find('\'', pos);
    if (pos == std::string::npos) {
        return "";
    }
    pos++;

    std::string out;
    while (pos < line.size()) {
        if (line[pos] == '\'') {
            // either the closing quote, or the start of a '\'' escape
            if (line.compare(pos, 4, "'\\''") == 0) {
                out += '\'';
                pos += 4;
                continue;
            }
            break;
        }
        out += line[pos];
        pos++;
    }
    return out;
}

} // namespace

std::optional<std::string> schedule_add(const std::string & time_hhmm, const std::string & prompt,
                                         const std::string & binary_path, const std::string & model_path,
                                         int ngl, const std::string & report_path) {
    static const std::regex time_re(R"(^([01]?\d|2[0-3]):([0-5]\d)$)");
    std::smatch m;
    if (!std::regex_match(time_hhmm, m, time_re)) {
        return std::nullopt;
    }
    int hour   = std::stoi(m[1]);
    int minute = std::stoi(m[2]);

    auto lines = current_crontab_lines();

    int next_n = 1;
    static const std::regex name_re(MARKER + R"(byte-(\d+))");
    for (const auto & line : lines) {
        std::smatch nm;
        if (std::regex_search(line, nm, name_re)) {
            next_n = std::max(next_n, std::stoi(nm[1]) + 1);
        }
    }
    std::string name = "byte-" + std::to_string(next_n);

    std::ostringstream cron_line;
    cron_line << minute << " " << hour << " * * * "
              << shell_quote(binary_path) << " -m " << shell_quote(model_path)
              << " -ngl " << ngl
              << " --batch " << shell_quote(prompt)
              << " --report " << shell_quote(report_path)
              << " " << MARKER << name;

    lines.push_back(cron_line.str());

    std::ostringstream out;
    for (const auto & line : lines) {
        out << line << "\n";
    }

    if (!write_crontab(out.str())) {
        return std::nullopt;
    }
    return name;
}

std::vector<scheduled_job> schedule_list() {
    std::vector<scheduled_job> jobs;

    static const std::regex job_re(R"(^(\d+)\s+(\d+)\s+\*\s+\*\s+\*\s+.*)" + MARKER + R"((byte-\d+))");

    for (const auto & line : current_crontab_lines()) {
        std::smatch m;
        if (!std::regex_search(line, m, job_re)) {
            continue;
        }

        scheduled_job job;
        int minute = std::stoi(m[1]);
        int hour   = std::stoi(m[2]);
        job.name   = m[3];

        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
        job.time = buf;

        job.prompt = extract_quoted_arg(line, "--batch");
        jobs.push_back(job);
    }
    return jobs;
}

bool schedule_remove(const std::string & name) {
    auto lines = current_crontab_lines();
    std::string marker = MARKER + name;

    size_t before = lines.size();
    lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const std::string & line) {
        // exact marker match, not just a prefix (byte-1 vs byte-10)
        size_t pos = line.find(marker);
        return pos != std::string::npos && pos + marker.size() == line.size();
    }), lines.end());

    if (lines.size() == before) {
        return false;
    }

    std::ostringstream out;
    for (const auto & line : lines) {
        out << line << "\n";
    }
    return write_crontab(out.str());
}
