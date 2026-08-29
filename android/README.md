# Byte AI Android port -- work in progress

Started on the `android-port` branch. The scaffold builds to a real debug APK (verified below); the app
itself is still the stock llama.android sample flow (pick a GGUF, chat) with Byte's system prompt/specs and
theming layered on -- not yet Byte's actual feature set (memory, tools, etc, see below).

## Building it

Same pattern as the desktop CLI (see the root README's Getting Started): this is a llama.cpp example, so it
needs to sit inside a llama.cpp checkout to resolve its native build.

```bash
git clone https://github.com/ggml-org/llama.cpp.git
cp -r Byte-AI-Tera/android/app-scaffold llama.cpp/examples/byte-android
cd llama.cpp/examples/byte-android
echo "sdk.dir=$ANDROID_HOME" > local.properties
./gradlew assembleDebug
```

Verified working with: JDK 17 (Gradle 8.14.3 doesn't yet support JDK 26), Android SDK platform 36 +
build-tools 36.1.0, NDK 30.0.14904198. Two real gotchas hit while getting a clean build:
- No `sdkmanager` was available to fetch the SDK's own bundled CMake package, so `cmake.dir=<path to a
  directory containing bin/cmake>` was added to `local.properties` (e.g. `cmake.dir=/opt/homebrew` for a
  Homebrew CMake install) to point at a system CMake instead.
- `lib/build.gradle.kts` originally pinned `version = "3.31.6"` in its `externalNativeBuild { cmake {} }`
  block, which AGP enforces as an exact match -- rejecting a newer system CMake (4.3.2) outright even with
  `cmake.dir` set. Removed the pin so it just uses whatever `cmake.dir` resolves to.

Output: `app/build/outputs/apk/debug/app-debug.apk` (~110MB debug build, bundling `arm64-v8a` + `x86_64`
plus every `ggml-cpu-*` microarchitecture variant unstripped -- normal for an unoptimized multi-ABI debug
build, not release-representative).

## Base

`app-scaffold/` is a copy of llama.cpp's official `examples/llama.android` (MIT licensed, same license as
the rest of llama.cpp) -- a real Gradle/Kotlin Android Studio project with a JNI bridge to llama.cpp's C
API. This replaces an earlier consideration of `timmyy123/LLM-Hub` as a base, which was ruled out: it's
under the PolyForm Noncommercial License 1.0.0, which explicitly bans app-store distribution and any
revenue-generating use unless a separate paid license is obtained from its author. llama.android carries no
such restriction.

## UI direction

`design-reference/ByteAI-3.0-ui-reference.html` is Byte 3.0 "Mega"'s original browser UI (pulled from
[Byte_AI@v3.1](https://github.com/RetroGigabyte/Byte_AI/blob/v3.1/ByteAI.html)), kept here purely as a
design reference for the Android UI -- not code to port directly (it's vanilla JS/HTML; the Android app
uses classic View-based layouts/XML resources, not Compose). The look carried forward so far: black
background, terminal-green (`#00ff00`) accents, monospace font -- matching Byte's CLI identity in
`Byte-AI-Tera` itself. Applied to `colors.xml`, `themes.xml`, `strings.xml` (app name -> "Byte AI"), the
message bubble drawables, and `activity_main.xml`/the two message item layouts (background, FAB tint,
text color, monospace font).

## App icon and model download

The launcher icon is now `Byte_AI.png` (adaptive icon: `ic_launcher_background.xml` is solid
`byte_black`, `ic_launcher_foreground.xml` insets the logo bitmap into the adaptive-icon safe zone).
The pre-model-load screen now has two buttons: the original folder icon (pick a local GGUF via the
system file picker) and a new download icon that fetches Byte's own model directly from
[Byte-AI-Models](https://github.com/RetroGigabyte/Byte-AI-Models) on GitHub -- same chunked/resumable/
sha256-verified approach as the desktop CLI's `/downloadmodel` (`modules/model_fetch.cpp`), reimplemented
in Kotlin with `HttpURLConnection` + `RandomAccessFile` rather than adding a new HTTP dependency. Requires
the `INTERNET` permission, now declared in `AndroidManifest.xml`.

## Ported tool modules

`Tools.kt` is a pure-Kotlin port of six of the desktop CLI's `modules/*.cpp` files -- no native/JNI changes,
same `HttpURLConnection` approach as the model downloader:
- **math_fetch, unit_fetch, quick_response**: computed directly and shown as-is, bypassing the model
  entirely, same as the desktop CLI's philosophy that small local models can't be trusted to relay even
  correct facts faithfully.
- **datetime_fetch**: reimplemented with `java.time`/real IANA timezones instead of porting the desktop's
  manual UTC-offset/DST bookkeeping -- `java.time` already handles DST correctly on its own.
- **weather_fetch, news_fetch, wiki_fetch**: fetched context is folded into the actual prompt sent to the
  model (mirroring the desktop's `turn_input` augmentation), so the model phrases the final answer using
  real fetched data rather than guessing.

All wired into `handleUserInput()` in `MainActivity.kt`, keyword-routed the same way as the desktop CLI's
interactive loop (deterministic tools checked first, then weather/news, then a Wikipedia fallback for
anything else that isn't a quick chitchat reply). `/model` is also implemented (typed in chat, intercepted
before it ever reaches the model, matching the desktop CLI's `/model`).

No local disk cache for Wikipedia lookups yet (the desktop CLI caches to `~/Byte/wiki-chat-cache.json`) --
every lookup on Android is a fresh network call.

## What still needs real work (not started)

- `app-scaffold/lib`'s JNI layer needs the model wired to Byte's actual GGUF (currently generic
  llama.android sample wiring).
- The Kotlin package name / applicationId is still the generic `com.example.llama` /
  `com.arm.aichat` from the sample -- not renamed yet since that touches more files and couldn't be
  verified without a build.
- Features that need real Android-specific infrastructure, not just a Kotlin rewrite:
  - `modules/scheduler.cpp` (crontab) -> `WorkManager`/`AlarmManager`
  - `modules/memory_store`, `category_fetch`, `training_log` (cross-session memory search, the curated
    knowledge base, and the training-corpus log) -> Android sandboxed storage; memory_store in particular
    used SQLite FTS5 on desktop and would need Android's built-in SQLite (or Room) for a real port
  - Saved/named conversations (`/save`, `/load`, `/namechat`, `/history`, `/delchat`), `/forget`
  - The `fastfetch`-backed `/specs` (see main `wiki-chat.cpp`) has no Android equivalent to shell out to;
    Android's specs are already gathered natively via `gatherAndroidSpecs()` in `MainActivity.kt` instead
  - Platform self-detection: bare `uname()` reports "Linux" on Android same as a Linux desktop (Android
    runs the Linux kernel) -- needs something Android-specific (e.g. checking for `/system/build.prop`, or
    just hardcoding it since a dedicated Android build already knows what it is). Not relevant to the
    Kotlin app itself since it never calls `uname()` -- only matters if native code ever needs it.
- The APK builds but hasn't been installed/run on a real device or emulator yet (no emulator available in
  the environment it was built in) -- next real step is side-loading the debug APK onto an actual arm64-v8a
  device (a Galaxy S26 Ultra is the intended first test device) to confirm it actually launches and chats.
