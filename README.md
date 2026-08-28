# Byte AI 4.0 "Tera"

**Your intelligent assistant that learns, remembers, and understands context.**

[![Version](https://img.shields.io/badge/version-4.0.0-blue.svg)](https://github.com/RetroGigabyte/Byte-AI-Tera)
[![Status](https://img.shields.io/badge/status-Beta%20Active-green.svg)](https://github.com/RetroGigabyte/Byte-AI-Tera)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

---

## What is Byte AI 4.0?

Byte AI is a comprehensive AI assistant, now running as a real local language model instead of hardcoded
browser JavaScript. Byte AI 4.0 "Tera" is a continuation of [Byte_AI](https://github.com/RetroGigabyte/Byte_AI)
(the browser-based 3.0 "Mega"), rebuilt on top of [llama.cpp](https://github.com/ggml-org/llama.cpp) running
Llama 3.2 1B locally on your own machine, GPU-accelerated where available.

**Get it:** clone this repo and build against llama.cpp (see [Getting Started](#getting-started)).

### Key Features
- **Conversation Context** - Understands conversation flow ("tell me more")
- **Real-Time News** - HackerNews & Dev.to feeds
- **Wikipedia Knowledge** - Smart learning from Wikipedia
- **Math & Utilities** - Direct math expressions, live time/date with timezone conversion
- **Local Training Log** - Everything Byte learns is saved for later fine-tuning
- **Runs Entirely Locally** - No server, no API key, no browser -- a native binary talking to your own GPU

---

## What's New in 4.0

### A Real Model, Not Hardcoded Responses
3.0 "Mega" ran on hand-written JavaScript with ~20 hardcoded responses. 4.0 "Tera" replaces that with an
actual local LLM (Llama 3.2 1B via llama.cpp), so answers are generated, not scripted -- while keeping every
knowledge tool 3.0 had, rebuilt in C++.

### Conversation Context Awareness
Byte still understands the flow of your conversation:

```
You: "What is Python?"
Byte: [explains Python]

You: "Tell me more"
Byte: [understands you mean more about Python]
```

**How it works:**
- Remembers previous conversation turns
- Extracts key topics automatically
- Fills in vague requests with context
- Natural conversation experience

### Real-Time News Integration
Stay updated with live news:

```
You: "HackerNews"
Byte: - Saving 100 terabytes of memory by optimizing 1.1.1.1's DNS cache
       519 pts | 8/27/2026, 1:17 PM
      - Small Models Have Arrived
       465 pts | 8/27/2026, 11:56 AM

You: "More"
Byte: [shows 20 stories instead of 10]
```

**Sources:**
- **HackerNews** - Top tech stories
- **Dev.to** - Developer articles

### Enhanced Features

#### Wikipedia Knowledge Base
- Smart sentence extraction
- Disk-backed cache with disambiguation-aware key building
- Conversation-context-aware follow-ups
- Facts treated as supplementary to the model's own knowledge, never overriding it

#### Math & Expressions
```
Direct: "2+3" → 2 + 3 = 5
Word-based: "What is 10 times 5?" → 10 * 5 = 50
```
Computed exactly in C++ and answered directly -- not left to the model to (mis)calculate.

#### Time & Date
```
"What time is it?" → Today is Thursday August 27 2026 at 10:11 PM Eastern time
"How about in Pacific time?" → Today is Thursday August 27 2026 at 7:11 PM Pacific time
```
Reads the system clock and timezone directly (auto-synced to whatever the OS is set to, no hardcoded
offset), with real UTC-offset math for conversions to other US timezones -- typo-tolerant, so "paffic time"
still resolves to Pacific.

#### Local Training Log
Every Wikipedia/news fact Byte fetches is appended to a local corpus (`wiki-chat-training.txt`), deduped, so
nothing is logged twice for the same cached topic. `retrain.sh` wraps llama.cpp's fine-tuning support to fold
that corpus back into the model between sessions.

#### Weather
```
"Weather in Tokyo" → Weather in Tokyo: 17 C, Light misty fog, humidity 77%, wind 9 km/h
```
Live conditions via wttr.in, defaulting to IP geolocation if no location is named.

#### Instant Quick Replies
Greetings, thanks, and "help" get an instant canned reply with no model generation pass at all, the same
as math/date-time -- e.g. "hi", "thanks", "help".

---

## Getting Started

Byte AI 4.0 is a llama.cpp example, so it needs a llama.cpp checkout to build against:

```bash
# Clone llama.cpp and drop Byte's source into its examples directory
git clone https://github.com/ggml-org/llama.cpp.git
git clone https://github.com/RetroGigabyte/Byte-AI-Tera.git
mkdir -p llama.cpp/examples/wiki-chat
cp Byte-AI-Tera/*.cpp Byte-AI-Tera/*.h Byte-AI-Tera/CMakeLists.txt Byte-AI-Tera/retrain.sh llama.cpp/examples/wiki-chat/
```

Add `wiki-chat` as a subdirectory in `llama.cpp/examples/CMakeLists.txt` -- this is the **only** line
touched anywhere in the llama.cpp tree itself (see [Integration Footprint](#integration-footprint) below);
`examples-CMakeLists.patch` in this repo is that exact one-line diff if you'd rather apply it directly:

```cmake
add_subdirectory(wiki-chat)
```

Build and run:

```bash
cd llama.cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target llama-wiki-chat -j

./build/bin/llama-wiki-chat -m /path/to/Llama-3.2-1B-Instruct.gguf -ngl 999
```

Grab a Llama 3.2 1B Instruct GGUF from e.g.
[bartowski/Llama-3.2-1B-Instruct-GGUF](https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF).

### CLI Flags

| Flag | Purpose | Default |
|------|---------|---------|
| `-m` | path to the model gguf (required) | -- |
| `-c` | context size | 4096 |
| `-ngl` | GPU layers to offload | 99 |
| `--cache` | Wikipedia cache file | `wiki-chat-cache.json` |
| `--train-log` | training corpus file | `wiki-chat-training.txt` |

---

## Usage Examples

### Knowledge Queries
```
"Eiffel Tower"
"Machine learning"
```

### Context-Aware
```
"What is Python?"
[Byte explains Python]

"Tell me more"
[Byte gives more Python info - no need to repeat]
```

### News & Headlines
```
"HackerNews"
"Dev.to"
"More" (after a news query, for 20 items instead of 10)
```

### Math & Utilities
```
"2+2"
"What is 10 times 5?"
"What time is it?"
"How about in Pacific time?"
```

### Identity
```
"Who are you?"
"What can you do?"
```

### Session Commands
```
/version   Show Byte AI version, llama.cpp build, and loaded model
/bye       Exit (also /quit, /end, /exit)
```

---

## Features Status

| Feature | Status | Details |
|---------|--------|---------|
| Conversation Context | Working | Multi-turn context awareness |
| Wikipedia | Working | Smart extraction, disk cache |
| HackerNews | Working | Top 10/20 stories with points |
| Dev.to | Working | Latest articles with reactions |
| Math | Working | Direct and word-based, computed exactly, model bypassed |
| Time/Date | Working | Real timezone conversion, typo-tolerant, model bypassed |
| Identity ("who are you") | Working | Bypasses tool lookups so it can't collide with an unrelated Wikipedia article |
| Local Training Log | Working | Deduped corpus of everything Byte has fetched |
| Fine-tuning fetched data back in | Experimental | Blocked on an upstream llama.cpp WIP bug, see Known Issues |
| GPU acceleration | Working | Metal on Apple Silicon, verified full layer offload |
| Weather | Working | wttr.in, location extraction or IP geolocation |
| Instant Quick Replies | Working | Greetings/thanks/help, model bypassed |

---

## Integration Footprint

Byte AI 4.0 lives entirely in its own `examples/wiki-chat/` directory once dropped into a llama.cpp
checkout. The **only** change made anywhere else in the llama.cpp tree is one line in
`examples/CMakeLists.txt` registering that new subdirectory -- no existing llama.cpp source file is
modified. That exact diff is checked in here as `examples-CMakeLists.patch`:

```diff
--- a/examples/CMakeLists.txt
+++ b/examples/CMakeLists.txt
@@ -31,6 +31,7 @@ else()
     add_subdirectory(simple-chat)
     add_subdirectory(speculative)
     add_subdirectory(speculative-simple)
+    add_subdirectory(wiki-chat)
     add_subdirectory(gen-docs)
     add_subdirectory(training)
     add_subdirectory(diffusion)
```

---

## Known Issues

### 1. Fine-tuning crashes on this llama.cpp build
**Issue:** `llama-finetune` (via `retrain.sh`) hits a `GGML_ASSERT` in `ggml_build_backward_expand` when run
against an F32 Llama 3.2 1B model.
- **Cause:** looks like Llama 3.2's tied input/output embeddings hit a view-op the backward pass doesn't
  support yet -- a genuine upstream WIP limitation (llama.cpp's own training docs call this "very much WIP"),
  not a bug in Byte's code.
- **Status:** data collection (`training_log`) works fine; only the actual retrain-and-reload step is
  currently blocked. Revisit when upstream's training support matures.

### 2. Small-model arithmetic/fact relay is unreliable
**Issue:** even when told the exact correct answer, a 1B model can still restate it wrong (observed: told
"2 + 3 = 5", it replied "2"; told the time was 10:07 PM, it replied 10:06 AM).
- **Fix applied:** math and date/time results now bypass the model entirely and answer directly, since both
  are deterministic facts where correctness matters more than a conversational restatement.

---

## Roadmap

### Next
- GitHub-hosted knowledge base distilled from a bigger model (e.g. Qwen2 3B), chunked to fit GitHub's file
  size limits, fetched dynamically the same way Wikipedia/news are

### Later
- Dictionary/definitions, unit conversion
- Speculative: Core ML / Apple Neural Engine inference (research spike only -- llama.cpp has no ANE backend
  today, and ANE has real disadvantages for autoregressive decoding vs. the Metal GPU path already in use)

---

## Technical Specifications

### Performance
- Verified full GPU offload on Apple Silicon (17/17 layers to Metal on an M4)
- Llama 3.2 1B Q4_K_M: ~800MB on disk

### Compatibility
- Anywhere llama.cpp builds: macOS, Linux, Windows
- Requires a llama.cpp checkout to build against (not a standalone binary distribution)

### Architecture
- **Language:** C++17
- **Inference:** [llama.cpp](https://github.com/ggml-org/llama.cpp) / ggml, GPU-accelerated via Metal/CUDA/etc.
  depending on platform
- **APIs:** Wikipedia REST API, HackerNews Firebase API, Dev.to API
- **HTTP/JSON:** libcurl, nlohmann::json
- **Storage:** flat JSON cache + plain-text training corpus on disk

---

## Development

### Project Structure
```
Byte-AI-Tera/
├── wiki-chat.cpp          # main chat loop, tool routing, Byte's system prompt
├── wiki_fetch.{h,cpp}     # Wikipedia REST API, cache, context-awareness
├── news_fetch.{h,cpp}     # HackerNews + Dev.to
├── datetime_fetch.{h,cpp} # system clock, timezone conversion
├── math_fetch.{h,cpp}     # exact arithmetic
├── training_log.{h,cpp}   # local training corpus collection
├── retrain.sh             # folds the corpus back into the model via llama-finetune
├── CMakeLists.txt         # builds llama-wiki-chat once dropped into llama.cpp/examples/
└── README.md              # this file
```

Each knowledge source follows the same pattern: a `*_is_requested(query)` detector and a `*_fetch(query)`
function. Results are injected into the user's turn as context before the chat-template prompt is built --
framed as supplementary (fill gaps, don't override) for Wikipedia/news, or as a direct known fact (state it,
don't recompute) for math/date-time, which bypass the model's generation step entirely for guaranteed
correctness.

### No External Config
- No API keys required (all data sources used are public, unauthenticated APIs)
- No server component -- a single native binary

### Version History
| Version | Focus |
|---------|-------|
| 4.0.0 "Tera" | Rebuilt on llama.cpp + Llama 3.2 1B; math and time/date now exact and model-independent |
| 3.0.0 "Mega" | Context awareness, News, Weather (browser JS) |
| 2.85 | Web version release |
| 2.75 | Killo DAT system |
| 1.0 | Initial release |

---

## Contributing

### Reporting Issues
Open a GitHub issue with:
- OS and how you built llama.cpp (backend: Metal/CUDA/CPU)
- Steps to reproduce
- Expected vs actual behavior

### Development Setup
```bash
# Edit source files directly, then re-copy into your llama.cpp checkout
cp *.cpp *.h llama.cpp/examples/wiki-chat/
cd llama.cpp
cmake --build build --target llama-wiki-chat -j

# Commit changes back in this repo
cd ../Byte-AI-Tera
git add -A
git commit -m "your message"
git push origin main
```

---

## License

MIT License - feel free to use, modify, and distribute.

---

## Attribution

### APIs & Services
- **Wikipedia REST API** - Knowledge base
- **HackerNews Firebase API** - Tech news
- **Dev.to API** - Developer articles

### Technologies
- [llama.cpp](https://github.com/ggml-org/llama.cpp) / ggml - local LLM inference
- libcurl, nlohmann::json
- No external dependencies beyond the above

---

## Quick Links

| Link | Purpose |
|------|---------|
| [Repository](https://github.com/RetroGigabyte/Byte-AI-Tera) | Source code |
| [Byte_AI (3.0 "Mega")](https://github.com/RetroGigabyte/Byte_AI) | The original browser-based predecessor |
| [Wiki-Fetch](https://github.com/RetroGigabyte/Wiki-Fetch) | The Wikipedia module this was ported from |
| [Issues](https://github.com/RetroGigabyte/Byte-AI-Tera/issues) | Bug reports |

---

**Byte AI 4.0 "Tera" - Intelligent. Contextual. Locally Yours.**
