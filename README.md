# Byte-AI-Tera

Byte AI 4.0 "Tera" — a continuation of [Byte_AI](https://github.com/RetroGigabyte/Byte_AI), rebuilt as a
[llama.cpp](https://github.com/ggml-org/llama.cpp) chat example running Llama 3.2 1B locally, instead of
hardcoded browser JS.

## Features

- **Wikipedia knowledge** (`wiki_fetch`) — REST API lookup, sentence extraction, disk cache, and
  conversation-context awareness ("tell me more"). Ported from
  [Wiki-Fetch](https://github.com/RetroGigabyte/Wiki-Fetch).
- **Live news** (`news_fetch`) — HackerNews and Dev.to feeds, with "more" for a longer list.
- **Date/time** (`datetime_fetch`) — reads the system clock and timezone directly (auto-synced, no
  hardcoded offset), and can convert to any other US timezone on request, with typo tolerance.
- **Math** (`math_fetch`) — direct expressions (`2+3`) and word-based ones (`what is 10 times 5`),
  computed exactly rather than left to the model.
- **Training log** (`training_log`) — every fetched Wikipedia/news fact is appended to a local corpus
  (`wiki-chat-training.txt`), which `retrain.sh` can fold back into the model via llama.cpp's
  (experimental/WIP) finetuning support.
- Byte's own identity via a system prompt, plus `/bye`, `/quit`, `/end`, `/exit`, and `/version`.

## Setup

This is a llama.cpp example, so it needs a llama.cpp checkout to build against:

```sh
git clone https://github.com/ggml-org/llama.cpp.git
cp *.cpp *.h CMakeLists.txt retrain.sh llama.cpp/examples/wiki-chat/
```

Then add `wiki-chat` as a subdirectory in `llama.cpp/examples/CMakeLists.txt`:

```cmake
add_subdirectory(wiki-chat)
```

Build and run:

```sh
cd llama.cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target llama-wiki-chat -j
./build/bin/llama-wiki-chat -m /path/to/model.gguf -ngl 999
```

Grab a Llama 3.2 1B Instruct GGUF from e.g.
[bartowski/Llama-3.2-1B-Instruct-GGUF](https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF).

## License

MIT, same as Byte_AI.
