// Byte AI 4.0 "Tera": a llama.cpp chat loop wired to the Byte AI DAT modules
// (https://github.com/RetroGigabyte/Byte_AI, https://github.com/RetroGigabyte/Wiki-Fetch).
// every user turn is checked against the news and Wikipedia modules first,
// and anything found is injected into the prompt as context before the
// model generates a response.

#include "llama.h"
#include "build-info.h"
#include "modules/category_fetch.h"
#include "modules/datetime_fetch.h"
#include "modules/math_fetch.h"
#include "modules/memory_store.h"
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
    "You are Byte, an AI assistant (Byte AI 4.0 \"Tera\"). You have access to live "
    "knowledge tools: a Wikipedia lookup, a HackerNews/Dev.to news feed, live weather data, "
    "the system clock (local date/time, synced to whatever timezone the machine is set to, "
    "including conversions to other US timezones), a calculator, a unit converter (length, "
    "weight, volume, speed, temperature), and a curated knowledge base on select topics. "
    "Wikipedia, news, weather, and curated-knowledge "
    "results are supplementary context, filling gaps in or checking facts against "
    "what you already know, never overriding your own judgment. The date/time and math "
    "results are direct facts computed for you, so state them as given rather than "
    "recomputing them yourself. Answer naturally and concisely.";

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

static void print_usage(int, char ** argv) {
    printf("\nexample usage:\n");
    printf("\n    %s -m model.gguf [-c context_size] [-ngl n_gpu_layers] [--cache path.json] [--train-log path.txt] [--memory-db path.db] [--report path.txt] [--batch \"prompt\"]\n", argv[0]);
    printf("\n");
}

int main(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");

    std::string model_path;
    std::string cache_path = "wiki-chat-cache.json";
    std::string train_log_path = "wiki-chat-training.txt";
    std::string memory_db_path = "wiki-chat-memory.db";
    std::string category_cache_path = "wiki-chat-category-cache.json";
    std::string report_path = "wiki-chat-reports.txt";
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
    llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.8f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    wiki_fetch wiki(cache_path);
    memory_store memory(memory_db_path);
    category_fetch categories(category_cache_path);
    int64_t session_id = memory.start_session();

    auto generate = [&](const std::string & prompt) {
        std::string response;

        const bool is_first = llama_memory_seq_pos_max(llama_get_memory(ctx), 0) == -1;

        const int n_prompt_tokens = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), NULL, 0, is_first, true);
        std::vector<llama_token> prompt_tokens(n_prompt_tokens);
        if (llama_tokenize(vocab, prompt.c_str(), prompt.size(), prompt_tokens.data(), prompt_tokens.size(), is_first, true) < 0) {
            GGML_ABORT("failed to tokenize the prompt\n");
        }

        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());
        llama_token new_token_id;
        while (true) {
            int n_ctx_cur  = llama_n_ctx(ctx);
            int n_ctx_used = llama_memory_seq_pos_max(llama_get_memory(ctx), 0) + 1;
            if (n_ctx_used + batch.n_tokens > n_ctx_cur) {
                printf("\033[0m\n");
                fprintf(stderr, "context size exceeded\n");
                exit(0);
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

            batch = llama_batch_get_one(&new_token_id, 1);
        }

        return response;
    };

    std::vector<llama_chat_message> messages;
    messages.push_back({"system", strdup(BYTE_SYSTEM_PROMPT)});

    std::vector<wiki_turn> history;
    std::string chat_name; // set via /namechat; lets a bare /save reuse it automatically
    bool secret_mode = false; // set via /secret; suppresses memory/training logging while on
    std::string user_name; // set via /user; folded into the system prompt so Byte addresses them by name

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
        std::string response = generate(prompt);
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
                printf("Byte AI 4.0 \"Tera\"\n");
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
                    printf("got it -- I'll call you %s\n", user_name.c_str());
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
            }
        } else if (weather_is_requested(user)) {
            auto weather = weather_fetch(user);
            if (weather) {
                printf("\033[36m[weather]\033[0m\n");
                turn_input = "A weather API was just called for this request and returned real, current "
                             "data (not something you need to disclaim): " + *weather +
                             ". Report it directly and naturally, with no hedging about data access.\n"
                             "User request: " + user;
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
            }
        } else if (categories.is_requested(user)) {
            auto result = categories.fetch(user);
            if (result) {
                printf("\033[36m[knowledge: %s]\033[0m\n", result->first.c_str());
                turn_input = "Curated knowledge on \"" + result->first + "\" (use this only to fill gaps or "
                             "check facts; otherwise answer from what you already know): " + result->second +
                             "\nUser question: " + user;
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
        std::string response = generate(prompt);
        printf("\n\033[0m");

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
            std::string summary = generate(prompt);
            printf("\n\033[0m");
            memory.set_session_summary(session_id, summary);
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
