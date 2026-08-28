#include "model_fetch.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <sys/stat.h>

using json = nlohmann::json;

namespace {

size_t curl_write_cb(char * ptr, size_t size, size_t nmemb, void * userdata) {
    auto * out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t curl_write_file_cb(char * ptr, size_t size, size_t nmemb, void * userdata) {
    auto * f = static_cast<FILE *>(userdata);
    return fwrite(ptr, size, nmemb, f);
}

std::optional<std::string> http_get_string(const std::string & url) {
    CURL * curl = curl_easy_init();
    if (!curl) {
        return std::nullopt;
    }
    std::string body;
    long status = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || status < 200 || status >= 300) {
        return std::nullopt;
    }
    return body;
}

// appends url's content to the already-open file f
bool http_append_to_file(const std::string & url, FILE * f) {
    CURL * curl = curl_easy_init();
    if (!curl) {
        return false;
    }
    long status = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_easy_cleanup(curl);
    return res == CURLE_OK && status >= 200 && status < 300;
}

int64_t file_size(const std::string & path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return -1;
    }
    return st.st_size;
}

// shells out to shasum/sha256sum rather than adding a crypto library
// dependency, matching the popen() pattern already used by modules/scheduler.cpp
std::optional<std::string> sha256_file(const std::string & path) {
    std::string cmd = "shasum -a 256 '" + path + "' 2>/dev/null || sha256sum '" + path + "' 2>/dev/null";
    FILE * pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return std::nullopt;
    }
    std::array<char, 256> buf;
    std::string out;
    if (fgets(buf.data(), buf.size(), pipe)) {
        out = buf.data();
    }
    pclose(pipe);

    size_t sp = out.find(' ');
    if (sp == std::string::npos) {
        return std::nullopt;
    }
    return out.substr(0, sp);
}

std::string base_url_of(const std::string & manifest_url) {
    size_t slash = manifest_url.find_last_of('/');
    return slash == std::string::npos ? "" : manifest_url.substr(0, slash + 1);
}

} // namespace

bool model_fetch(const std::string & manifest_url, const std::string & output_path) {
    auto body = http_get_string(manifest_url);
    if (!body) {
        fprintf(stderr, "model_fetch: failed to fetch manifest from %s\n", manifest_url.c_str());
        return false;
    }

    json manifest;
    try {
        manifest = json::parse(*body);
    } catch (const std::exception &) {
        fprintf(stderr, "model_fetch: manifest did not parse as JSON\n");
        return false;
    }

    if (!manifest.contains("chunks") || !manifest.contains("sha256")) {
        fprintf(stderr, "model_fetch: manifest is missing required fields\n");
        return false;
    }

    std::string base_url    = base_url_of(manifest_url);
    std::string expected_sha = manifest["sha256"];

    // resume support: skip chunks whose bytes are already fully present at
    // the right offset in output_path; truncate back to the first chunk
    // boundary that isn't fully downloaded so appends stay aligned
    int64_t existing = file_size(output_path);
    if (existing < 0) {
        existing = 0;
    }

    uint64_t offset = 0;
    size_t   resume_index = 0;
    for (const auto & chunk : manifest["chunks"]) {
        uint64_t size = chunk.value("size", 0ULL);
        if ((uint64_t) existing >= offset + size) {
            offset += size;
            resume_index++;
        } else {
            break;
        }
    }

    FILE * f = fopen(output_path.c_str(), resume_index > 0 ? "r+b" : "wb");
    if (!f) {
        fprintf(stderr, "model_fetch: could not open %s for writing\n", output_path.c_str());
        return false;
    }
    if (fseek(f, (long) offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    size_t n_chunks = manifest["chunks"].size();
    for (size_t i = resume_index; i < n_chunks; i++) {
        const auto & chunk = manifest["chunks"][i];
        std::string name = chunk.value("name", "");
        printf("  [%zu/%zu] fetching %s\n", i + 1, n_chunks, name.c_str());
        fflush(stdout);

        if (!http_append_to_file(base_url + name, f)) {
            fprintf(stderr, "model_fetch: failed to download chunk %s (re-run to resume)\n", name.c_str());
            fclose(f);
            return false;
        }
        fflush(f);
    }
    fclose(f);

    printf("verifying checksum...\n");
    fflush(stdout);
    auto actual_sha = sha256_file(output_path);
    if (!actual_sha || *actual_sha != expected_sha) {
        fprintf(stderr, "model_fetch: checksum mismatch (got %s, expected %s)\n",
                actual_sha ? actual_sha->c_str() : "(none)", expected_sha.c_str());
        return false;
    }

    return true;
}
