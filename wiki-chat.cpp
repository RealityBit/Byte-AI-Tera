// Byte AI 4.0 "Tera": a llama.cpp chat loop wired to the Byte AI DAT modules
// (https://github.com/RetroGigabyte/Byte_AI, https://github.com/RetroGigabyte/Wiki-Fetch).
// every user turn is checked against the news and Wikipedia modules first,
// and anything found is injected into the prompt as context before the
// model generates a response.

#include "llama.h"
#include "build-info.h"
#include "datetime_fetch.h"
#include "math_fetch.h"
#include "news_fetch.h"
#include "training_log.h"
#include "wiki_fetch.h"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
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

static const char * BYTE_SYSTEM_PROMPT =
    "You are Byte, an AI assistant (Byte AI 4.0 \"Tera\"). You have access to live "
    "knowledge tools: a Wikipedia lookup, a HackerNews/Dev.to news feed, the system clock "
    "(local date/time, synced to whatever timezone the machine is set to, including "
    "conversions to other US timezones), and a calculator. Wikipedia and news results are "
    "supplementary context, filling gaps in or checking facts against what you already know, "
    "never overriding your own judgment. The date/time and math results are direct facts "
    "computed for you, so state them as given rather than recomputing them yourself. Answer "
    "naturally and concisely.";

static void print_usage(int, char ** argv) {
    printf("\nexample usage:\n");
    printf("\n    %s -m model.gguf [-c context_size] [-ngl n_gpu_layers] [--cache path.json] [--train-log path.txt]\n", argv[0]);
    printf("\n");
}

int main(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");

    std::string model_path;
    std::string cache_path = "wiki-chat-cache.json";
    std::string train_log_path = "wiki-chat-training.txt";
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
        }

        std::string turn_input = user;

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

        if (is_identity_question(user)) {
            // fall through with no tool lookup; the system prompt already covers this
        } else if (news_is_requested(user)) {
            auto news = news_fetch(user);
            if (news) {
                printf("\033[36m[news]\033[0m\n");
                training_log_append("News: " + user, *news, train_log_path);
                turn_input = "Live news feed just fetched for the user's request (you have no other way to "
                             "know these current headlines, so present this list to them, verbatim titles, "
                             "as your answer):\n" + *news +
                             "\nUser request: " + user;
            }
        } else if (!is_chitchat(user)) {
            auto wiki_result = wiki.learn(user, history);
            if (wiki_result) {
                std::string context = wiki_fetch::format_response(
                    std::accumulate(wiki_result->sentences.begin(), wiki_result->sentences.end(), std::string(),
                        [](const std::string & acc, const std::string & s) { return acc.empty() ? s : acc + " " + s; }));

                printf("\033[36m[wiki: %s]\033[0m\n", wiki_result->title.c_str());
                training_log_append(wiki_result->title, context, train_log_path);

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

        messages.push_back({"assistant", strdup(response.c_str())});
        prev_len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), false, nullptr, 0);
        if (prev_len < 0) {
            fprintf(stderr, "failed to apply the chat template\n");
            return 1;
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
