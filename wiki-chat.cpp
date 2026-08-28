// Byte AI 4.0 "Tera": a llama.cpp chat loop wired to the Byte AI DAT modules
// (https://github.com/RetroGigabyte/Byte_AI, https://github.com/RetroGigabyte/Wiki-Fetch).
// every user turn is checked against the news and Wikipedia modules first,
// and anything found is injected into the prompt as context before the
// model generates a response.

#include "llama.h"
#include "build-info.h"
#include "modules/category_fetch.h"
#include "modules/datetime_fetch.h"
#include "modules/logo_banner.h"
#include "modules/math_fetch.h"
#include "modules/memory_store.h"
#include "modules/model_fetch.h"
#include "modules/news_fetch.h"
#include "modules/quick_response.h"
#include "modules/scheduler.h"
#include "modules/training_log.h"
#include "modules/unit_fetch.h"
#include "modules/weather_fetch.h"
#include "modules/wiki_fetch.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <climits>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static int levenshtein(const std::string & a, const std::string & b) {
    std::vector<std::vector<int>> d(a.size() + 1, std::vector<int>(b.size() + 1));
    for (size_t i = 0; i <= a.size(); i++) d[i][0] = (int) i;
    for (size_t j = 0; j <= b.size(); j++) d[0][j] = (int) j;
    for (size_t i = 1; i <= a.size(); i++) {
        for (size_t j = 1; j <= b.size(); j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
        }
    }
    return d[a.size()][b.size()];
}

static const std::vector<std::string> & known_commands() {
    static const std::vector<std::string> cmds = {
        "/bye", "/quit", "/end", "/exit", "/version", "/ver", "/model", "/user", "/history",
        "/knowledge", "/namechat", "/save", "/load", "/forget", "/delchat", "/newchat",
        "/secret", "/schedule", "/schedules", "/unschedule", "/wipecfg", "/downloadmodel",
        "/listmods", "/switchmod", "/help",
    };
    return cmds;
}

// short conversational turns that are not knowledge lookups, so a Wikipedia
// fetch should not be attempted even if the word happens to have an article
// (e.g. plain "hello" would otherwise pull up the etymology of the greeting)
static bool is_chitchat(const std::string & user) {
    static const std::set<std::string> phrases = {
        "hi", "hello", "hey", "yo", "sup",
        "thanks", "thank you", "thx", "ty",
        "bye", "goodbye", "see ya", "later",
        "yes", "no", "yeah", "yep", "nope", "ok", "okay", "sure", "cool", "nice",
        "lol", "lmao", "haha",
    };

    std::string trimmed = user;
    size_t a = trimmed.find_first_not_of(" \t\r\n");
    size_t b = trimmed.find_last_not_of(" \t\r\n.!?");
    trimmed = (a == std::string::npos) ? "" : trimmed.substr(a, b - a + 1);

    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

    if (phrases.count(lower)) {
        return true;
    }

    // a single short word with no question mark reads as chitchat, not a topic lookup
    bool has_space = lower.find(' ') != std::string::npos;
    if (!has_space && lower.size() <= 5 && trimmed.find('?') == std::string::npos) {
        return true;
    }

    return false;
}

// questions about the assistant itself ("who are you") should be answered from the
// system prompt, not derailed by a Wikipedia article that happens to share the title
// (e.g. "Who Are You" is a song by The Who)
static bool is_identity_question(const std::string & user) {
    static const std::vector<std::string> patterns = {
        "who are you", "what are you", "what is byte", "who is byte",
        "what can you do", "what do you do", "tell me about yourself",
    };

    std::string lower = user;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

    for (const auto & p : patterns) {
        if (lower.find(p) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// phrases that ask Byte to recall something from a past session, not just
// the current conversation -- inspired by hermes-agent's cross-session FTS5
// recall (https://github.com/NousResearch/hermes-agent)
static bool memory_is_requested(const std::string & user) {
    static const std::vector<std::string> patterns = {
        "remember", "recall", "we talked about", "you mentioned",
        "earlier you said", "last time", "before you said", "did i tell you",
    };

    std::string lower = user;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

    for (const auto & p : patterns) {
        if (lower.find(p) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static const char * BYTE_SYSTEM_PROMPT =
    "You are Byte, an AI assistant (Byte AI 4.0 \"Tera\"). You are made up of several modules "
    "working together, and you should describe yourself accurately using them when asked what "
    "you can do:\n"
    "- Live knowledge tools: a Wikipedia lookup, a HackerNews/Dev.to news feed, live weather "
    "data, and a curated knowledge base on select topics, distilled from a bigger model and "
    "fetched on demand. Their results are supplementary context, filling gaps in or checking "
    "facts against what you already know, never overriding your own judgment.\n"
    "- Exact-computation tools: a calculator, a unit converter (length, weight, volume, speed, "
    "temperature), and the system clock (local date/time, synced to whatever timezone the "
    "machine is set to, including conversions to other US timezones). Their results are direct "
    "facts computed for you, so state them as given rather than recomputing them yourself.\n"
    "- Cross-session memory: every conversation is logged to a local database, searchable across "
    "sessions, not just the current one -- when past-conversation snippets are given to you as "
    "context, they are genuinely something you recalled, not something you're inventing.\n"
    "- Session features available to the user directly (not something you invoke yourself, but "
    "you can mention them if relevant): saving/loading/naming conversations, forgetting specific "
    "remembered topics, scheduling recurring automated prompts, and switching which underlying "
    "model is loaded.\n"
    "\n"
    "Most of the time a relevant tool's result is already given to you above, if one applies. But "
    "if you genuinely need one of: wiki, news, weather, math, unit, datetime, knowledge -- and none "
    "was already provided -- you may request it yourself. To do that, reply with ONLY this exact "
    "line and nothing else: TOOL: <name> <query>  (e.g. \"TOOL: weather Tokyo\", \"TOOL: wiki Eiffel "
    "Tower\"). The wiki tool in particular is a real, working Wikipedia lookup you have direct access "
    "to -- use it whenever a question is about a specific real-world person, place, thing, or event "
    "and no Wikipedia context was already given to you, rather than answering from memory alone or "
    "declining to answer. Do not say you lack real-time or lookup access when this tool is available "
    "to you; request it instead. Only do this when "
    "you truly cannot answer without it; never combine it with other text, and never do this for "
    "something you already know or that has already been supplied to you.\n"
    "\n"
    "If you are not confident in a factual answer -- a specific name, date, number, fact about a "
    "real person/place/thing/event, or anything you'd be guessing at -- look it up with the "
    "appropriate tool before answering, rather than stating an uncertain guess as if it were fact. "
    "Being wrong with confidence is worse than taking one extra step to check. This applies "
    "especially to wiki for real-world entities, knowledge for general facts, and news/weather for "
    "anything current. Only skip the lookup when you are genuinely certain, or when a tool's result "
    "was already given to you above.\n"
    "Answer naturally and concisely.";

// strips anything that could escape the ~/Byte directory (path separators,
// leading dots) so /save can't be used to write outside it
static std::string sanitize_filename(const std::string & name) {
    std::string out;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == '\0') {
            continue;
        }
        out += c;
    }
    while (!out.empty() && out.front() == '.') {
        out.erase(out.begin());
    }
    return out;
}

// ~/Byte/config.json: persists settings (currently just /user's name) across
// sessions, independent of any saved chat
static std::optional<std::string> config_path() {
    const char * home = getenv("HOME");
    if (!home) {
        return std::nullopt;
    }
    std::string dir = std::string(home) + "/Byte";
    mkdir(dir.c_str(), 0755);
    return dir + "/config.json";
}

// builds the default path for one of Byte's data files under ~/Byte/, so
// cache/memory/training/report files all live in one place instead of
// scattered across whatever directory the binary happened to be launched
// from. falls back to a plain relative filename if $HOME isn't set
static std::string default_byte_path(const std::string & filename) {
    const char * home = getenv("HOME");
    if (!home) {
        return filename;
    }
    std::string dir = std::string(home) + "/Byte";
    mkdir(dir.c_str(), 0755);
    return dir + "/" + filename;
}

static std::string load_config_user_name() {
    auto path = config_path();
    if (!path) {
        return "";
    }
    std::ifstream in(*path);
    if (!in.is_open()) {
        return "";
    }
    try {
        nlohmann::json j;
        in >> j;
        return j.value("user_name", "");
    } catch (const std::exception &) {
        return "";
    }
}

static void save_config_user_name(const std::string & name) {
    auto path = config_path();
    if (!path) {
        return;
    }
    std::ofstream out(*path);
    if (out.is_open()) {
        out << nlohmann::json{{"user_name", name}}.dump(2);
    }
}

static bool wipe_config() {
    auto path = config_path();
    return path && remove(path->c_str()) == 0;
}

// saves the current conversation to ~/Byte/<name>.Byte_Mem as JSON
static bool save_chat(const std::string & name, const std::vector<wiki_turn> & history) {
    std::string clean = sanitize_filename(name);
    if (clean.empty()) {
        return false;
    }

    const char * home = getenv("HOME");
    if (!home) {
        return false;
    }

    std::string dir = std::string(home) + "/Byte";
    mkdir(dir.c_str(), 0755);

    nlohmann::json j;
    j["version"] = "Byte AI 4.0 \"Tera\"";
    j["turns"]   = nlohmann::json::array();
    for (const auto & turn : history) {
        j["turns"].push_back({{"user", turn.user}, {"bot", turn.bot}});
    }

    std::ofstream out(dir + "/" + clean + ".Byte_Mem");
    if (!out.is_open()) {
        return false;
    }
    out << j.dump(2);
    return true;
}

// loads a conversation previously written by save_chat, or nullopt if the
// file doesn't exist / doesn't parse
static std::optional<std::vector<wiki_turn>> load_chat(const std::string & name) {
    std::string clean = sanitize_filename(name);
    if (clean.empty()) {
        return std::nullopt;
    }

    const char * home = getenv("HOME");
    if (!home) {
        return std::nullopt;
    }

    std::ifstream in(std::string(home) + "/Byte/" + clean + ".Byte_Mem");
    if (!in.is_open()) {
        return std::nullopt;
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception &) {
        return std::nullopt;
    }

    if (!j.contains("turns") || !j["turns"].is_array()) {
        return std::nullopt;
    }

    std::vector<wiki_turn> turns;
    for (const auto & t : j["turns"]) {
        turns.push_back({t.value("user", ""), t.value("bot", "")});
    }
    return turns;
}

// lists every saved chat's name (without the .Byte_Mem suffix) in ~/Byte
static std::vector<std::string> list_saved_chats() {
    std::vector<std::string> names;

    const char * home = getenv("HOME");
    if (!home) {
        return names;
    }

    DIR * dir = opendir((std::string(home) + "/Byte").c_str());
    if (!dir) {
        return names;
    }

    static const std::string suffix = ".Byte_Mem";
    while (dirent * entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            names.push_back(name.substr(0, name.size() - suffix.size()));
        }
    }
    closedir(dir);

    std::sort(names.begin(), names.end());
    return names;
}

// lists "name:tag" for every model Ollama has already pulled, by walking its
// manifest directory directly (no dependency on the ollama daemon running)
static std::vector<std::string> list_ollama_models() {
    std::vector<std::string> out;
    const char * home = getenv("HOME");
    if (!home) {
        return out;
    }

    std::string base = std::string(home) + "/.ollama/models/manifests/registry.ollama.ai";
    DIR * ns_dir = opendir(base.c_str());
    if (!ns_dir) {
        return out;
    }
    while (dirent * ns_entry = readdir(ns_dir)) {
        std::string ns = ns_entry->d_name;
        if (ns == "." || ns == "..") continue;

        std::string ns_path = base + "/" + ns;
        DIR * name_dir = opendir(ns_path.c_str());
        if (!name_dir) continue;
        while (dirent * name_entry = readdir(name_dir)) {
            std::string name = name_entry->d_name;
            if (name == "." || name == "..") continue;

            std::string name_path = ns_path + "/" + name;
            DIR * tag_dir = opendir(name_path.c_str());
            if (!tag_dir) continue;
            while (dirent * tag_entry = readdir(tag_dir)) {
                std::string tag = tag_entry->d_name;
                if (tag == "." || tag == "..") continue;
                out.push_back(name + ":" + tag);
            }
            closedir(tag_dir);
        }
        closedir(name_dir);
    }
    closedir(ns_dir);

    std::sort(out.begin(), out.end());
    return out;
}

// resolves an Ollama model tag ("qwen2.5:3b", or "phi3.5" for :latest) to the
// actual GGUF blob Ollama already downloaded, without needing the ollama
// daemon or a re-download -- reads the manifest the same way `ollama list`
// would, and follows its "model" layer digest to the content-addressed blob
static std::optional<std::string> resolve_ollama_model(const std::string & tag_in) {
    const char * home = getenv("HOME");
    if (!home) {
        return std::nullopt;
    }

    std::string name = tag_in, tag = "latest";
    size_t colon = tag_in.find(':');
    if (colon != std::string::npos) {
        name = tag_in.substr(0, colon);
        tag  = tag_in.substr(colon + 1);
    }

    std::string manifest_path = std::string(home) + "/.ollama/models/manifests/registry.ollama.ai/library/" + name + "/" + tag;
    std::ifstream in(manifest_path);
    if (!in.is_open()) {
        return std::nullopt;
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception &) {
        return std::nullopt;
    }

    if (!j.contains("layers")) {
        return std::nullopt;
    }
    for (const auto & layer : j["layers"]) {
        if (layer.value("mediaType", "") != "application/vnd.ollama.image.model") {
            continue;
        }
        std::string digest = layer.value("digest", "");
        size_t sep = digest.find(':');
        if (sep == std::string::npos) {
            return std::nullopt;
        }
        std::string blob_path = std::string(home) + "/.ollama/models/blobs/sha256-" + digest.substr(sep + 1);
        struct stat st;
        if (stat(blob_path.c_str(), &st) == 0) {
            return blob_path;
        }
    }
    return std::nullopt;
}

static void print_usage(int, char ** argv) {
    printf("\nexample usage:\n");
    printf("\n    %s -m model.gguf [-c context_size] [-ngl n_gpu_layers] [--cache path.json] [--train-log path.txt] [--memory-db path.db] [--report path.txt] [--batch \"prompt\"] [--knowledge-budget-mb N]\n", argv[0]);
    printf("\n");
}

int main(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");

    std::string model_path;
    std::string cache_path = default_byte_path("wiki-chat-cache.json");
    std::string train_log_path = default_byte_path("wiki-chat-training.txt");
    std::string memory_db_path = default_byte_path("wiki-chat-memory.db");
    std::string category_cache_path = default_byte_path("wiki-chat-category-cache.json");
    uint64_t knowledge_budget_mb = 0; // 0 = unbounded
    std::string report_path = default_byte_path("wiki-chat-reports.txt");
    std::optional<std::string> batch_prompt;
    int ngl   = 99;
    int n_ctx = 4096;

    for (int i = 1; i < argc; i++) {
        try {
            if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
                model_path = argv[++i];
            } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
                n_ctx = std::stoi(argv[++i]);
            } else if (strcmp(argv[i], "-ngl") == 0 && i + 1 < argc) {
                ngl = std::stoi(argv[++i]);
            } else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) {
                cache_path = argv[++i];
            } else if (strcmp(argv[i], "--train-log") == 0 && i + 1 < argc) {
                train_log_path = argv[++i];
            } else if (strcmp(argv[i], "--memory-db") == 0 && i + 1 < argc) {
                memory_db_path = argv[++i];
            } else if (strcmp(argv[i], "--category-cache") == 0 && i + 1 < argc) {
                category_cache_path = argv[++i];
            } else if (strcmp(argv[i], "--knowledge-budget-mb") == 0 && i + 1 < argc) {
                knowledge_budget_mb = std::stoull(argv[++i]);
            } else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
                report_path = argv[++i];
            } else if (strcmp(argv[i], "--batch") == 0 && i + 1 < argc) {
                batch_prompt = argv[++i];
            } else {
                print_usage(argc, argv);
                return 1;
            }
        } catch (std::exception & e) {
            fprintf(stderr, "error: %s\n", e.what());
            print_usage(argc, argv);
            return 1;
        }
    }
    if (model_path.empty()) {
        print_usage(argc, argv);
        return 1;
    }

    // resolved to absolute paths so /schedule's installed cron line still
    // works regardless of cron's own working directory (which is typically
    // not the directory this was launched from)
    std::string binary_path = model_path; // placeholder, overwritten below
    {
        char resolved[PATH_MAX];
        if (realpath(argv[0], resolved)) {
            binary_path = resolved;
        }
        if (realpath(model_path.c_str(), resolved)) {
            model_path = resolved;
        }
        // report_path's file may not exist yet, so realpath() (which requires
        // the target to exist) can't be used -- just prepend cwd if relative
        if (!report_path.empty() && report_path[0] != '/' && getcwd(resolved, sizeof(resolved))) {
            report_path = std::string(resolved) + "/" + report_path;
        }
    }

    llama_log_set([](enum ggml_log_level level, const char * text, void * /* user_data */) {
        if (level >= GGML_LOG_LEVEL_ERROR) {
            fprintf(stderr, "%s", text);
        }
    }, nullptr);

    ggml_backend_load_all();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = ngl;

    llama_model * model = llama_model_load_from_file(model_path.c_str(), model_params);
    if (!model) {
        fprintf(stderr, "%s: error: unable to load model\n", __func__);
        return 1;
    }

    const llama_vocab * vocab = llama_model_get_vocab(model);

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx   = n_ctx;
    ctx_params.n_batch = n_ctx;

    llama_context * ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) {
        fprintf(stderr, "%s: error: failed to create the llama_context\n", __func__);
        return 1;
    }

    llama_sampler * smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    // penalizes recently-used tokens so small models are less likely to fall
    // into a degenerate repetition loop (observed: Phi-3.5-mini looping
    // forever re-deriving the same correct numbers while insisting they were wrong)
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), 256, 1.1f, 0.0f, 0.0f));
    llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.8f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    wiki_fetch wiki(cache_path);
    memory_store memory(memory_db_path);
    category_fetch categories(category_cache_path, knowledge_budget_mb * 1024ULL * 1024ULL);
    int64_t session_id = memory.start_session();

    // thrown by generate() when the running conversation has filled the
    // context window. caught by the caller, which resets the chat (like
    // /newchat) instead of the old behavior of exit()ing the whole process --
    // a single long response shouldn't end the entire session
    struct context_overflow {};

    auto generate = [&](const std::string & prompt, int max_response_tokens) {
        std::string response;

        const bool is_first = llama_memory_seq_pos_max(llama_get_memory(ctx), 0) == -1;

        const int n_prompt_tokens = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), NULL, 0, is_first, true);
        std::vector<llama_token> prompt_tokens(n_prompt_tokens);
        if (llama_tokenize(vocab, prompt.c_str(), prompt.size(), prompt_tokens.data(), prompt_tokens.size(), is_first, true) < 0) {
            GGML_ABORT("failed to tokenize the prompt\n");
        }

        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());
        llama_token new_token_id;
        // a hard cap independent of context size: even with the repetition
        // penalty above, a small model can still get stuck re-deriving the
        // same (sometimes already-correct) answer forever. give up cleanly
        // rather than run until context overflow. the caller picks the cap --
        // factual/tool-assisted answers get a tight one, open chat gets more room
        int n_generated = 0;
        while (true) {
            int n_ctx_cur  = llama_n_ctx(ctx);
            int n_ctx_used = llama_memory_seq_pos_max(llama_get_memory(ctx), 0) + 1;
            if (n_ctx_used + batch.n_tokens > n_ctx_cur) {
                printf("\033[0m\n");
                fprintf(stderr, "context size exceeded\n");
                throw context_overflow{};
            }

            int ret = llama_decode(ctx, batch);
            if (ret != 0) {
                GGML_ABORT("failed to decode, ret = %d\n", ret);
            }

            new_token_id = llama_sampler_sample(smpl, ctx, -1);

            if (llama_vocab_is_eog(vocab, new_token_id)) {
                break;
            }

            char buf[256];
            int n = llama_token_to_piece(vocab, new_token_id, buf, sizeof(buf), 0, true);
            if (n < 0) {
                GGML_ABORT("failed to convert token to piece\n");
            }
            std::string piece(buf, n);
            printf("%s", piece.c_str());
            fflush(stdout);
            response += piece;

            if (++n_generated >= max_response_tokens) {
                printf("\n\033[36m[gave up -- response was running too long / repeating]\033[0m");
                fflush(stdout);
                return std::string("I can't answer this properly :(");
            }

            batch = llama_batch_get_one(&new_token_id, 1);
        }

        return response;
    };

    std::vector<llama_chat_message> messages;
    messages.push_back({"system", strdup(BYTE_SYSTEM_PROMPT)});

    std::vector<wiki_turn> history;
    std::string chat_name; // set via /namechat; lets a bare /save reuse it automatically
    bool secret_mode = false; // set via /secret; suppresses memory/training logging while on
    std::string user_name = load_config_user_name(); // persisted in ~/Byte/config.json across sessions

    // rebuilds messages[0] from BYTE_SYSTEM_PROMPT plus the user's name, if set.
    // like the other system-prompt edits in this codebase, this only affects
    // prompts built from here on -- context already decoded into the KV cache
    // keeps whatever system prompt was in effect at the time
    auto update_system_prompt = [&]() {
        std::string prompt = BYTE_SYSTEM_PROMPT;
        if (!user_name.empty()) {
            prompt += " The user you're talking to is named " + user_name + "; address them by name naturally.";
        }
        free(const_cast<char *>(messages[0].content));
        messages[0].content = strdup(prompt.c_str());
    };
    if (!user_name.empty()) {
        update_system_prompt(); // picked up from ~/Byte/config.json at startup
    }
    std::vector<char> formatted(llama_n_ctx(ctx));
    int prev_len = 0;

    // for facts that are exact and deterministic (math, the system clock), answer
    // directly instead of asking the model to relay them -- even when told the
    // answer, a 1B model can still garble it (observed: told "2 + 3 = 5", it
    // replied "2"; told the time was 10:06 PM, it replied "10:06 AM"). this turn
    // is never decoded into the model's KV cache, so a later follow-up referencing
    // it won't be in context -- an accepted tradeoff for guaranteed correctness
    auto answer_directly = [&](const std::string & user, const std::string & answer) {
        printf("\033[33m%s\033[0m\n", answer.c_str());

        if (!secret_mode) {
            memory.log_turn(session_id, "user", user);
            memory.log_turn(session_id, "assistant", answer);
        }

        history.push_back({user, answer});

        const char * tmpl = llama_model_chat_template(model, /* name */ nullptr);
        messages.push_back({"user", strdup(user.c_str())});
        messages.push_back({"assistant", strdup(answer.c_str())});
        prev_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), false, nullptr, 0);
        if (prev_len < 0) {
            fprintf(stderr, "failed to apply the chat template\n");
            exit(1);
        }
    };

    // decodes an already-known (user, response) turn straight into the KV cache
    // instead of sampling it, so a loaded conversation (/load) actually resumes
    // in-context rather than just being replayed in the on-screen transcript
    auto prime_turn = [&](const std::string & user, const std::string & response) -> bool {
        const char * tmpl = llama_model_chat_template(model, /* name */ nullptr);

        messages.push_back({"user", strdup(user.c_str())});
        int new_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), true, formatted.data(), formatted.size());
        if (new_len > (int) formatted.size()) {
            formatted.resize(new_len);
            new_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), true, formatted.data(), formatted.size());
        }
        if (new_len < 0) {
            return false;
        }
        std::string prompt(formatted.begin() + prev_len, formatted.begin() + new_len);

        const bool is_first = llama_memory_seq_pos_max(llama_get_memory(ctx), 0) == -1;

        const int n_prompt_tokens = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), NULL, 0, is_first, true);
        std::vector<llama_token> prompt_tokens(n_prompt_tokens);
        llama_tokenize(vocab, prompt.c_str(), prompt.size(), prompt_tokens.data(), prompt_tokens.size(), is_first, true);

        const int n_resp_tokens = -llama_tokenize(vocab, response.c_str(), response.size(), NULL, 0, false, true);
        std::vector<llama_token> resp_tokens(n_resp_tokens);
        llama_tokenize(vocab, response.c_str(), response.size(), resp_tokens.data(), resp_tokens.size(), false, true);

        std::vector<llama_token> all_tokens = prompt_tokens;
        all_tokens.insert(all_tokens.end(), resp_tokens.begin(), resp_tokens.end());

        int n_ctx_cur  = llama_n_ctx(ctx);
        int n_ctx_used = llama_memory_seq_pos_max(llama_get_memory(ctx), 0) + 1;
        if (n_ctx_used + (int) all_tokens.size() > n_ctx_cur) {
            return false;
        }

        llama_batch batch = llama_batch_get_one(all_tokens.data(), all_tokens.size());
        if (llama_decode(ctx, batch) != 0) {
            return false;
        }

        messages.push_back({"assistant", strdup(response.c_str())});
        prev_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), false, nullptr, 0);
        return prev_len >= 0;
    };

    // clears conversation state (messages, history, KV cache) and starts a
    // fresh memory session, optionally naming it -- shared by /newchat and
    // /namechat, since naming a chat starts a new one rather than renaming
    // whatever's currently in progress
    auto start_new_chat = [&](const std::string & name) {
        for (size_t i = 1; i < messages.size(); i++) {
            free(const_cast<char *>(messages[i].content));
        }
        messages.resize(1);
        history.clear();
        prev_len = 0;
        llama_memory_clear(llama_get_memory(ctx), true);

        session_id  = memory.start_session();
        chat_name   = name; // empty clears it, same as no name given
        secret_mode = false;

        if (name.empty()) {
            printf("started a new chat\n");
        } else {
            printf("started a new chat named \"%s\"\n", sanitize_filename(name).c_str());
        }
    };

    // hot-swaps the loaded model without restarting the process. conversation
    // state tied to the old model's tokenizer/KV cache can't carry over to a
    // different model, so this resets the chat the same way /newchat does
    auto switch_model = [&](const std::string & path) -> bool {
        llama_model_params new_model_params = llama_model_default_params();
        new_model_params.n_gpu_layers = ngl;

        llama_model * new_model = llama_model_load_from_file(path.c_str(), new_model_params);
        if (!new_model) {
            return false;
        }

        llama_context_params new_ctx_params = llama_context_default_params();
        new_ctx_params.n_ctx   = n_ctx;
        new_ctx_params.n_batch = n_ctx;

        llama_context * new_ctx = llama_init_from_model(new_model, new_ctx_params);
        if (!new_ctx) {
            llama_model_free(new_model);
            return false;
        }

        llama_sampler * new_smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(new_smpl, llama_sampler_init_penalties(
            llama_vocab_n_tokens(llama_model_get_vocab(new_model)), 256, 1.1f, 0.0f, 0.0f));
        llama_sampler_chain_add(new_smpl, llama_sampler_init_min_p(0.05f, 1));
        llama_sampler_chain_add(new_smpl, llama_sampler_init_temp(0.8f));
        llama_sampler_chain_add(new_smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

        llama_sampler_free(smpl);
        llama_free(ctx);
        llama_model_free(model);

        model = new_model;
        ctx   = new_ctx;
        smpl  = new_smpl;
        vocab = llama_model_get_vocab(model);

        char resolved[PATH_MAX];
        model_path = realpath(path.c_str(), resolved) ? resolved : path;

        for (size_t i = 1; i < messages.size(); i++) {
            free(const_cast<char *>(messages[i].content));
        }
        messages.resize(1);
        history.clear();
        prev_len = 0;
        formatted.assign((size_t) llama_n_ctx(ctx), 0);
        update_system_prompt();

        return true;
    };

    // parses a model-initiated tool request, only when the model's ENTIRE
    // trimmed response is exactly "TOOL: <name> <query>" -- requiring the
    // whole response to match avoids false positives from the phrase turning
    // up inside ordinary prose
    auto parse_tool_request = [](const std::string & response) -> std::optional<std::pair<std::string, std::string>> {
        std::string trimmed = response;
        size_t a = trimmed.find_first_not_of(" \t\r\n");
        size_t b = trimmed.find_last_not_of(" \t\r\n");
        trimmed = (a == std::string::npos) ? "" : trimmed.substr(a, b - a + 1);

        if (trimmed.rfind("TOOL:", 0) != 0) {
            return std::nullopt;
        }
        std::string rest = trimmed.substr(5);
        size_t sp = rest.find_first_not_of(' ');
        rest = (sp == std::string::npos) ? "" : rest.substr(sp);

        // only take the first line as the query, even if the model kept
        // generating past the "TOOL: ..." line instead of stopping there as
        // instructed -- otherwise trailing rambling becomes part of the query
        // (observed: "TOOL: wiki Tesla\n<paragraph>" sent the whole paragraph
        // to the Wikipedia lookup as if it were the search term)
        size_t nl = rest.find('\n');
        if (nl != std::string::npos) {
            rest = rest.substr(0, nl);
        }
        size_t end = rest.find_last_not_of(" \t\r");
        rest = (end == std::string::npos) ? "" : rest.substr(0, end + 1);

        size_t sep = rest.find(' ');
        if (sep == std::string::npos) {
            return std::nullopt;
        }
        return std::make_pair(rest.substr(0, sep), rest.substr(sep + 1));
    };

    // executes a model-requested tool by name, reusing the same modules the
    // keyword router uses -- this is the "add alongside, don't replace"
    // version: the model can only reach for a tool the keyword router didn't
    // already fire for this turn
    auto execute_tool_by_name = [&](const std::string & name, const std::string & query) -> std::optional<std::string> {
        if (name == "math")     return math_fetch(query);
        if (name == "unit")     return unit_fetch(query);
        if (name == "datetime") return datetime_fetch(query);
        // weather_fetch extracts the location via a "weather in/for/at <place>"
        // regex, so a bare model-provided location string like "Tokyo" needs
        // wrapping first -- otherwise it silently falls back to IP-based
        // auto-location (observed: asked for Tokyo, got weather for Calhoun)
        if (name == "weather")  return weather_fetch("weather in " + query);
        if (name == "news")     return news_fetch(query);
        if (name == "knowledge") {
            auto result = categories.fetch(query);
            return result ? std::optional<std::string>(result->second) : std::nullopt;
        }
        if (name == "wiki") {
            auto result = wiki.learn(query, history);
            if (!result) {
                return std::nullopt;
            }
            return wiki_fetch::format_response(
                std::accumulate(result->sentences.begin(), result->sentences.end(), std::string(),
                    [](const std::string & acc, const std::string & s) { return acc.empty() ? s : acc + " " + s; }));
        }
        return std::nullopt;
    };

    // --batch mode: a single non-interactive turn, meant to be re-invoked by
    // cron via /schedule. routes through the same tool detectors as the
    // interactive loop, generates one reply, appends it to --report, and exits
    if (batch_prompt) {
        const std::string & user = *batch_prompt;
        std::string turn_input = user;

        if (auto result = unit_fetch(user)) {
            turn_input = "Computed unit conversion (state this directly): " + *result;
        } else if (auto result = math_fetch(user)) {
            turn_input = "Computed result (state this directly): " + *result;
        } else if (datetime_is_requested(user)) {
            turn_input = "Current date/time (state this directly): " + datetime_fetch(user);
        } else if (weather_is_requested(user)) {
            if (auto weather = weather_fetch(user)) {
                turn_input = "Live weather data just fetched (report it directly, no hedging): " + *weather +
                             "\nUser request: " + user;
            }
        } else if (news_is_requested(user)) {
            if (auto news = news_fetch(user)) {
                turn_input = "Live news feed just fetched (present this list verbatim as your answer):\n" + *news +
                             "\nUser request: " + user;
            }
        } else if (categories.is_requested(user)) {
            if (auto result = categories.fetch(user)) {
                turn_input = "Curated knowledge on \"" + result->first + "\" (use this only to fill gaps or "
                             "check facts; otherwise answer from what you already know): " + result->second +
                             "\nUser question: " + user;
            }
        } else if (auto wiki_result = wiki.learn(user, {})) {
            std::string context = wiki_fetch::format_response(
                std::accumulate(wiki_result->sentences.begin(), wiki_result->sentences.end(), std::string(),
                    [](const std::string & acc, const std::string & s) { return acc.empty() ? s : acc + " " + s; }));
            turn_input = "Supplementary Wikipedia context on \"" + wiki_result->title + "\": " + context +
                         "\n\nUser question: " + user;
        }

        messages.push_back({"user", strdup(turn_input.c_str())});
        const char * tmpl = llama_model_chat_template(model, /* name */ nullptr);
        int new_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), true, formatted.data(), formatted.size());
        if (new_len > (int) formatted.size()) {
            formatted.resize(new_len);
            new_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), true, formatted.data(), formatted.size());
        }
        if (new_len < 0) {
            fprintf(stderr, "failed to apply the chat template\n");
            return 1;
        }

        std::string prompt(formatted.begin() + prev_len, formatted.begin() + new_len);
        std::string response;
        try {
            response = generate(prompt, 400);
        } catch (context_overflow &) {
            fprintf(stderr, "context overflow on a single --batch turn -- try a shorter prompt\n");
            llama_sampler_free(smpl);
            llama_free(ctx);
            llama_model_free(model);
            return 1;
        }
        printf("\n");

        memory.log_turn(session_id, "user", user);
        memory.log_turn(session_id, "assistant", response);

        std::ofstream report(report_path, std::ios::app);
        if (report.is_open()) {
            report << "### " << datetime_fetch("") << "\n" << user << "\n\n" << response << "\n\n";
        }

        // same cleanup order as the interactive path's exit below -- skipping
        // this before returning left ggml-metal's static teardown crashing at exit
        for (auto & msg : messages) {
            free(const_cast<char *>(msg.content));
        }
        llama_sampler_free(smpl);
        llama_free(ctx);
        llama_model_free(model);

        return 0;
    }

    while (true) {
        printf("\033[32m> \033[0m");
        std::string user;
        std::getline(std::cin, user);

        if (user.empty()) {
            break;
        }

        {
            std::string lower = user;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            if (lower == "/bye" || lower == "/quit" || lower == "/end" || lower == "/exit") {
                printf("Bye!\n");
                break;
            }
            if (lower == "/version" || lower == "/ver") {
                printf("%s", BYTE_LOGO_BANNER);
                printf("Byte AI 4.0 \"Tera\"\n");
                continue;
            }
            if (lower == "/help") {
                printf(
                    "Byte AI 4.0 \"Tera\" -- commands:\n"
                    "\n"
                    "  Session\n"
                    "    /bye, /quit, /end, /exit   exit\n"
                    "    /version, /ver              logo + version\n"
                    "    /model                      llama.cpp build + loaded model path\n"
                    "    /help                        this list\n"
                    "\n"
                    "  Identity & config\n"
                    "    /user <name>                tell Byte your name (persisted)\n"
                    "    /wipecfg                     reset persisted settings\n"
                    "\n"
                    "  Conversations\n"
                    "    /namechat <name>             start a new, named chat\n"
                    "    /newchat [name]               start a fresh chat\n"
                    "    /save [name]                  save the current chat\n"
                    "    /load <name>                  resume a saved chat\n"
                    "    /delchat <name>               delete a saved chat\n"
                    "    /history                      list saved chats\n"
                    "    /secret                       toggle: stop logging this session\n"
                    "\n"
                    "  Memory\n"
                    "    /forget <topic>               forget matching past turns\n"
                    "    /forget all                   wipe all cross-session memory\n"
                    "\n"
                    "  Knowledge base\n"
                    "    /knowledge                    show local knowledge-cache usage\n"
                    "\n"
                    "  Models\n"
                    "    /listmods                     list local + Ollama models\n"
                    "    /switchmod <name>              hot-swap the loaded model\n"
                    "    /downloadmodel                 fetch Phi-3.5-mini from Byte-AI-Models\n"
                    "\n"
                    "  Automation\n"
                    "    /schedule <HH:MM> <prompt>    schedule a daily prompt\n"
                    "    /schedules                    list scheduled jobs\n"
                    "    /unschedule <name>             remove a scheduled job\n"
                    "\n"
                    "  Otherwise, just talk to Byte -- it knows Wikipedia, live news/weather, math,\n"
                    "  unit conversion, and the current date/time on its own.\n"
                );
                continue;
            }
            if (lower == "/model") {
                printf("llama.cpp build %d (%s), %s\n", llama_build_number(), llama_commit(), llama_build_target());
                printf("model: %s\n", model_path.c_str());
                continue;
            }
            if (lower.rfind("/user", 0) == 0) {
                std::string name = user.size() > 5 ? user.substr(5) : "";
                size_t a = name.find_first_not_of(' ');
                name = (a == std::string::npos) ? "" : name.substr(a);

                if (name.empty()) {
                    printf("usage: /user <name>\n");
                } else {
                    user_name = name;
                    update_system_prompt();
                    save_config_user_name(user_name);
                    printf("got it -- I'll call you %s (saved to ~/Byte/config.json)\n", user_name.c_str());
                }
                continue;
            }
            if (lower == "/wipecfg") {
                bool removed = wipe_config();
                user_name.clear();
                update_system_prompt();
                if (removed) {
                    printf("config wiped -- ~/Byte/config.json removed\n");
                } else {
                    printf("no config file to wipe\n");
                }
                continue;
            }
            if (lower == "/downloadmodel") {
                std::string dir = default_byte_path("models");
                mkdir(dir.c_str(), 0755);
                std::string out_path = dir + "/phi3.5-mini.gguf";

                printf("downloading phi3.5-mini (MIT licensed) to %s ...\n", out_path.c_str());
                bool ok = model_fetch(
                    "https://raw.githubusercontent.com/RetroGigabyte/Byte-AI-Models/main/manifest.json",
                    out_path);

                if (ok) {
                    printf("done -- verified checksum. Launch with -m %s to use it.\n", out_path.c_str());
                } else {
                    printf("download failed or was incomplete -- run /downloadmodel again to resume\n");
                }
                continue;
            }
            if (lower == "/listmods") {
                std::string dir = default_byte_path("models");
                DIR * d = opendir(dir.c_str());
                bool any = false;

                printf("current: %s\n", model_path.c_str());
                if (d) {
                    while (dirent * entry = readdir(d)) {
                        std::string name = entry->d_name;
                        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".gguf") != 0) {
                            continue;
                        }
                        any = true;
                        std::string full = dir + "/" + name;
                        struct stat st;
                        double mb = stat(full.c_str(), &st) == 0 ? st.st_size / (1024.0 * 1024.0) : 0.0;
                        printf("  %-40s %8.0f MB%s\n", name.c_str(), mb,
                               full == model_path ? "  (loaded)" : "");
                    }
                    closedir(d);
                }
                if (!any) {
                    printf("no models in ~/Byte/models -- try /downloadmodel\n");
                }

                auto ollama_models = list_ollama_models();
                if (!ollama_models.empty()) {
                    printf("ollama:\n");
                    for (const auto & tag : ollama_models) {
                        printf("  %s\n", tag.c_str());
                    }
                }
                continue;
            }
            if (lower.rfind("/switchmod", 0) == 0) {
                std::string name = user.size() > 10 ? user.substr(10) : "";
                size_t a = name.find_first_not_of(' ');
                name = (a == std::string::npos) ? "" : name.substr(a);

                if (name.empty()) {
                    printf("usage: /switchmod <name-or-path> (see /listmods for names)\n");
                    continue;
                }

                std::string dir = default_byte_path("models");
                std::vector<std::string> candidates = {name, dir + "/" + name, dir + "/" + name + ".gguf"};
                std::string resolved_path;
                for (const auto & c : candidates) {
                    struct stat st;
                    if (stat(c.c_str(), &st) == 0) {
                        resolved_path = c;
                        break;
                    }
                }
                if (resolved_path.empty()) {
                    if (auto ollama_path = resolve_ollama_model(name)) {
                        resolved_path = *ollama_path;
                    }
                }

                if (resolved_path.empty()) {
                    printf("no model found matching \"%s\" -- see /listmods\n", name.c_str());
                } else {
                    printf("switching to %s ...\n", resolved_path.c_str());
                    if (switch_model(resolved_path)) {
                        printf("now using %s (conversation reset -- different model, different context)\n", model_path.c_str());
                    } else {
                        printf("failed to load %s -- staying on the previous model\n", resolved_path.c_str());
                    }
                }
                continue;
            }
            if (lower == "/history") {
                auto chats = list_saved_chats();
                if (chats.empty()) {
                    printf("no saved chats yet -- use /save <name> to save one\n");
                } else {
                    printf("saved chats (~/Byte):\n");
                    for (const auto & name : chats) {
                        printf("  %s\n", name.c_str());
                    }
                }
                continue;
            }
            if (lower == "/knowledge") {
                uint64_t used = categories.cache_size_bytes();
                printf("knowledge cache: %.1f KB used", used / 1024.0);
                if (knowledge_budget_mb > 0) {
                    printf(" of %llu MB budget", (unsigned long long) knowledge_budget_mb);
                } else {
                    printf(" (unbounded)");
                }
                printf("\n");
                continue;
            }
            if (lower.rfind("/namechat", 0) == 0) {
                std::string name = user.size() > 9 ? user.substr(9) : "";
                size_t a = name.find_first_not_of(' ');
                name = (a == std::string::npos) ? "" : name.substr(a);

                if (name.empty()) {
                    printf("usage: /namechat <name> -- starts a completely new, named chat\n");
                } else {
                    start_new_chat(name);
                }
                continue;
            }
            if (lower.rfind("/save", 0) == 0) {
                std::string name = user.size() > 5 ? user.substr(5) : "";
                size_t a = name.find_first_not_of(' ');
                name = (a == std::string::npos) ? "" : name.substr(a);

                if (name.empty()) {
                    name = chat_name; // fall back to the name set via /namechat, if any
                } else {
                    chat_name = name; // an explicit name also becomes the chat's name going forward
                }

                if (name.empty()) {
                    printf("usage: /save <filename> (or /namechat <name> first, then bare /save)\n");
                } else if (save_chat(name, history)) {
                    printf("saved to ~/Byte/%s.Byte_Mem\n", sanitize_filename(name).c_str());
                } else {
                    printf("failed to save chat\n");
                }
                continue;
            }
            if (lower.rfind("/load", 0) == 0) {
                std::string name = user.size() > 5 ? user.substr(5) : "";
                size_t a = name.find_first_not_of(' ');
                name = (a == std::string::npos) ? "" : name.substr(a);

                if (name.empty()) {
                    printf("usage: /load <filename>\n");
                    continue;
                }

                auto loaded = load_chat(name);
                if (!loaded) {
                    printf("no saved chat found at ~/Byte/%s.Byte_Mem\n", sanitize_filename(name).c_str());
                    continue;
                }

                size_t loaded_count = 0;
                for (const auto & turn : *loaded) {
                    if (!prime_turn(turn.user, turn.bot)) {
                        printf("stopped loading early: ran out of context space\n");
                        break;
                    }
                    history.push_back(turn);
                    if (!secret_mode) {
                        memory.log_turn(session_id, "user", turn.user);
                        memory.log_turn(session_id, "assistant", turn.bot);
                    }
                    loaded_count++;
                }
                printf("loaded %zu turn(s) from ~/Byte/%s.Byte_Mem\n", loaded_count, sanitize_filename(name).c_str());
                continue;
            }
            if (lower.rfind("/forget", 0) == 0) {
                std::string arg = user.size() > 7 ? user.substr(7) : "";
                size_t a = arg.find_first_not_of(' ');
                arg = (a == std::string::npos) ? "" : arg.substr(a);
                std::string arg_lower = arg;
                std::transform(arg_lower.begin(), arg_lower.end(), arg_lower.begin(), [](unsigned char c) { return std::tolower(c); });

                if (arg_lower == "all" || arg_lower == "everything") {
                    int removed = memory.forget_all();
                    printf("forgot everything (%d turn(s) removed)\n", removed);
                } else if (arg.empty()) {
                    printf("usage: /forget <topic> (or /forget all to wipe everything remembered)\n");
                } else {
                    int removed = memory.forget(arg);
                    printf("forgot %d matching turn(s) about \"%s\"\n", removed, arg.c_str());
                }
                continue;
            }
            if (lower.rfind("/delchat", 0) == 0) {
                std::string name = user.size() > 8 ? user.substr(8) : "";
                size_t a = name.find_first_not_of(' ');
                name = (a == std::string::npos) ? "" : name.substr(a);
                std::string clean = sanitize_filename(name);

                const char * home = getenv("HOME");
                if (clean.empty()) {
                    printf("usage: /delchat <name>\n");
                } else if (!home) {
                    printf("failed to delete chat\n");
                } else {
                    std::string path = std::string(home) + "/Byte/" + clean + ".Byte_Mem";
                    if (remove(path.c_str()) == 0) {
                        printf("deleted ~/Byte/%s.Byte_Mem\n", clean.c_str());
                        if (clean == sanitize_filename(chat_name)) {
                            chat_name.clear();
                        }
                    } else {
                        printf("no saved chat found at ~/Byte/%s.Byte_Mem\n", clean.c_str());
                    }
                }
                continue;
            }
            if (lower.rfind("/newchat", 0) == 0) {
                std::string name = user.size() > 8 ? user.substr(8) : "";
                size_t a = name.find_first_not_of(' ');
                name = (a == std::string::npos) ? "" : name.substr(a);

                start_new_chat(name);
                continue;
            }
            if (lower == "/secret") {
                secret_mode = !secret_mode;
                if (secret_mode) {
                    printf("secret mode on -- this conversation won't be remembered or logged\n");
                } else {
                    printf("secret mode off -- back to normal logging\n");
                }
                continue;
            }
            if (lower.rfind("/schedule ", 0) == 0 || lower == "/schedule") {
                std::string rest = user.size() > 9 ? user.substr(9) : "";
                size_t a = rest.find_first_not_of(' ');
                rest = (a == std::string::npos) ? "" : rest.substr(a);

                size_t sp = rest.find(' ');
                std::string time_arg = sp == std::string::npos ? rest : rest.substr(0, sp);
                std::string prompt   = sp == std::string::npos ? "" : rest.substr(sp + 1);

                if (time_arg.empty() || prompt.empty()) {
                    printf("usage: /schedule <HH:MM> <prompt> (24-hour, runs daily)\n");
                } else if (auto name = schedule_add(time_arg, prompt, binary_path, model_path, ngl, report_path)) {
                    printf("scheduled \"%s\" daily at %s (job %s) -- results append to %s\n",
                           prompt.c_str(), time_arg.c_str(), name->c_str(), report_path.c_str());
                } else {
                    printf("failed to schedule -- check the time format (HH:MM, 24-hour)\n");
                }
                continue;
            }
            if (lower == "/schedules") {
                auto jobs = schedule_list();
                if (jobs.empty()) {
                    printf("no scheduled jobs -- use /schedule <HH:MM> <prompt> to add one\n");
                } else {
                    printf("scheduled jobs:\n");
                    for (const auto & job : jobs) {
                        printf("  %s  daily %s  \"%s\"\n", job.name.c_str(), job.time.c_str(), job.prompt.c_str());
                    }
                }
                continue;
            }
            if (lower.rfind("/unschedule", 0) == 0) {
                std::string name = user.size() > 11 ? user.substr(11) : "";
                size_t a = name.find_first_not_of(' ');
                name = (a == std::string::npos) ? "" : name.substr(a);

                if (name.empty()) {
                    printf("usage: /unschedule <job-name> (see /schedules for names)\n");
                } else if (schedule_remove(name)) {
                    printf("removed scheduled job %s\n", name.c_str());
                } else {
                    printf("no scheduled job named %s\n", name.c_str());
                }
                continue;
            }

            // none of the commands above matched -- if this looks like a slash
            // command at all, say so instead of silently falling through to
            // plain chat (a typo like "/verson" used to just get answered as
            // if it were a regular message, with no hint anything went wrong)
            if (!lower.empty() && lower[0] == '/') {
                size_t sp = lower.find(' ');
                std::string typed = sp == std::string::npos ? lower : lower.substr(0, sp);

                std::string closest;
                int best_dist = INT_MAX;
                for (const auto & cmd : known_commands()) {
                    int dist = levenshtein(typed, cmd);
                    if (dist < best_dist) {
                        best_dist = dist;
                        closest   = cmd;
                    }
                }

                if (best_dist <= 2) {
                    printf("unknown command %s -- did you mean %s?\n", typed.c_str(), closest.c_str());
                } else {
                    printf("unknown command %s\n", typed.c_str());
                }
                continue;
            }
        }

        std::string turn_input = user;

        if (unit_is_requested(user)) {
            auto result = unit_fetch(user);
            if (result) {
                printf("\033[36m[unit]\033[0m\n");
                answer_directly(user, *result);
                continue;
            }
        }

        if (math_is_requested(user)) {
            auto result = math_fetch(user);
            if (result) {
                printf("\033[36m[math]\033[0m\n");
                answer_directly(user, *result);
                continue;
            }
        }

        if (datetime_is_requested(user)) {
            printf("\033[36m[datetime]\033[0m\n");
            answer_directly(user, datetime_fetch(user));
            continue;
        }

        // instant canned replies for greetings/acknowledgements, same reasoning as
        // math/datetime: no need to spend a generation pass on "hi" or "thanks"
        if (auto quick = quick_response(user, user_name)) {
            answer_directly(user, *quick);
            continue;
        }

        // tool-assisted turns get a tighter cap -- they're meant to relay a
        // fact concisely, not write an essay. open-ended chat gets more room
        int response_cap = 512;

        if (is_identity_question(user)) {
            // fall through with no tool lookup; the system prompt already covers this
        } else if (memory_is_requested(user)) {
            auto hits = memory.search(user, 5);
            if (!hits.empty()) {
                printf("\033[36m[memory]\033[0m\n");
                std::string context;
                for (const auto & hit : hits) {
                    context += "[" + hit.started_at + "] " + hit.role + ": " + hit.content + "\n";
                }
                turn_input = "Snippets recalled from past conversations (use these only if relevant to "
                             "the current request; they may be about unrelated topics):\n" + context +
                             "\nUser request: " + user;
                response_cap = 350;
            }
        } else if (weather_is_requested(user)) {
            auto weather = weather_fetch(user);
            if (weather) {
                printf("\033[36m[weather]\033[0m\n");
                turn_input = "A weather API was just called for this request and returned real, current "
                             "data (not something you need to disclaim): " + *weather +
                             ". Report it directly and naturally, with no hedging about data access.\n"
                             "User request: " + user;
                response_cap = 350;
            }
        } else if (news_is_requested(user)) {
            auto news = news_fetch(user);
            if (news) {
                printf("\033[36m[news]\033[0m\n");
                if (!secret_mode) {
                    training_log_append("News: " + user, *news, train_log_path);
                }
                turn_input = "Live news feed just fetched for the user's request (you have no other way to "
                             "know these current headlines, so present this list to them, verbatim titles, "
                             "as your answer):\n" + *news +
                             "\nUser request: " + user;
                response_cap = 350;
            }
        } else if (categories.is_requested(user)) {
            auto result = categories.fetch(user);
            if (result) {
                printf("\033[36m[knowledge: %s]\033[0m\n", result->first.c_str());
                turn_input = "Curated knowledge on \"" + result->first + "\" (use this only to fill gaps or "
                             "check facts; otherwise answer from what you already know): " + result->second +
                             "\nUser question: " + user;
                response_cap = 350;
            }
        } else if (!is_chitchat(user)) {
            auto wiki_result = wiki.learn(user, history);
            if (wiki_result) {
                std::string context = wiki_fetch::format_response(
                    std::accumulate(wiki_result->sentences.begin(), wiki_result->sentences.end(), std::string(),
                        [](const std::string & acc, const std::string & s) { return acc.empty() ? s : acc + " " + s; }));

                printf("\033[36m[wiki: %s]\033[0m\n", wiki_result->title.c_str());
                if (!secret_mode) {
                    training_log_append(wiki_result->title, context, train_log_path);
                }

                turn_input = "Supplementary Wikipedia context on \"" + wiki_result->title + "\" (use this only to "
                             "fill gaps or check current facts your own knowledge might be missing; otherwise "
                             "answer from what you already know): " + context +
                             "\n\nUser question: " + user;
                response_cap = 350;
            }
        }

        history.push_back({user, ""});

        const char * tmpl = llama_model_chat_template(model, /* name */ nullptr);

        messages.push_back({"user", strdup(turn_input.c_str())});
        int new_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), true, formatted.data(), formatted.size());
        if (new_len > (int) formatted.size()) {
            formatted.resize(new_len);
            new_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), true, formatted.data(), formatted.size());
        }
        if (new_len < 0) {
            fprintf(stderr, "failed to apply the chat template\n");
            return 1;
        }

        std::string prompt(formatted.begin() + prev_len, formatted.begin() + new_len);

        printf("\033[33m");
        std::string response;
        try {
            response = generate(prompt, response_cap);
        } catch (context_overflow &) {
            printf("\n\033[0m");
            free(const_cast<char *>(messages.back().content));
            messages.pop_back();
            history.pop_back();
            printf("\033[36m[context full -- starting a fresh chat automatically]\033[0m\n");
            start_new_chat(chat_name);
            continue;
        }
        printf("\n\033[0m");

        // model-initiated tool calling: only reachable when the keyword router
        // didn't already fire a tool this turn (response_cap == 512 is the
        // "nothing matched" default) -- added alongside keyword routing, not
        // in place of it, so already-reliable cases are untouched
        if (response_cap == 512) {
            if (auto tool_req = parse_tool_request(response)) {
                // commit the TOOL: request itself as this turn's assistant
                // message, matching what's already decoded into the KV cache
                messages.push_back({"assistant", strdup(response.c_str())});
                prev_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), false, nullptr, 0);
                if (prev_len < 0) {
                    fprintf(stderr, "failed to apply the chat template\n");
                    return 1;
                }

                auto tool_result = execute_tool_by_name(tool_req->first, tool_req->second);
                if (!tool_result) {
                    printf("\033[36m[tool: %s -- no result]\033[0m\n", tool_req->first.c_str());
                    response = "I tried to look that up but couldn't get a result.";
                } else {
                    printf("\033[36m[tool: %s]\033[0m\n", tool_req->first.c_str());
                    std::string followup = "Tool result for \"" + tool_req->second + "\": " + *tool_result +
                                            "\nNow answer the user's original question using this.";
                    messages.push_back({"user", strdup(followup.c_str())});
                    int new_len2 = llama_chat_apply_template(tmpl, messages.data(), messages.size(), true, formatted.data(), formatted.size());
                    if (new_len2 > (int) formatted.size()) {
                        formatted.resize(new_len2);
                        new_len2 = llama_chat_apply_template(tmpl, messages.data(), messages.size(), true, formatted.data(), formatted.size());
                    }
                    if (new_len2 < 0) {
                        fprintf(stderr, "failed to apply the chat template\n");
                        return 1;
                    }
                    std::string prompt2(formatted.begin() + prev_len, formatted.begin() + new_len2);
                    printf("\033[33m");
                    try {
                        response = generate(prompt2, 350);
                    } catch (context_overflow &) {
                        printf("\n\033[0m");
                        free(const_cast<char *>(messages.back().content));
                        messages.pop_back();
                        history.pop_back();
                        printf("\033[36m[context full -- starting a fresh chat automatically]\033[0m\n");
                        start_new_chat(chat_name);
                        continue;
                    }
                    printf("\n\033[0m");
                }
            }
        }

        history.back().bot = response;
        if (!secret_mode) {
            memory.log_turn(session_id, "user", user);
            memory.log_turn(session_id, "assistant", response);
        }

        messages.push_back({"assistant", strdup(response.c_str())});
        prev_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), false, nullptr, 0);
        if (prev_len < 0) {
            fprintf(stderr, "failed to apply the chat template\n");
            return 1;
        }
    }

    // one last generation pass to summarize the session for cross-session
    // recall later (memory_store), instead of only ever storing raw turns.
    // skipped entirely in secret mode, so nothing about it is ever persisted
    if (!history.empty() && !secret_mode) {
        const char * tmpl = llama_model_chat_template(model, /* name */ nullptr);
        messages.push_back({"user", strdup("Summarize this entire conversation in one or two sentences, "
                                            "for your own future reference. Output only the summary.")});
        int new_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), true, formatted.data(), formatted.size());
        if (new_len > (int) formatted.size()) {
            formatted.resize(new_len);
            new_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), true, formatted.data(), formatted.size());
        }
        if (new_len >= 0) {
            std::string prompt(formatted.begin() + prev_len, formatted.begin() + new_len);
            printf("\033[36m[memory] saving session summary\033[0m\n\033[33m");
            try {
                std::string summary = generate(prompt, 150);
                memory.set_session_summary(session_id, summary);
            } catch (context_overflow &) {
                // not worth resetting anything for -- we're exiting right after this anyway
            }
            printf("\n\033[0m");
        }
    }

    for (auto & msg : messages) {
        free(const_cast<char *>(msg.content));
    }
    llama_sampler_free(smpl);
    llama_free(ctx);
    llama_model_free(model);

    return 0;
}
