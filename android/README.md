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

## What still needs real work (not started)

- `app-scaffold/lib`'s JNI layer needs the model wired to Byte's actual GGUF (currently generic
  llama.android sample wiring).
- The Kotlin package name / applicationId is still the generic `com.example.llama` /
  `com.arm.aichat` from the sample -- not renamed yet since that touches more files and couldn't be
  verified without a build.
- Everything in `Byte-AI-Tera`'s desktop modules that assumes desktop POSIX paths or shells out to a CLI
  tool needs an Android-native equivalent, built separately rather than assumed to just recompile:
  - `modules/scheduler.cpp` (crontab) -> `WorkManager`/`AlarmManager`
  - `modules/memory_store`, `category_fetch`, `wiki_fetch`, `training_log` -> Android sandboxed storage,
    JNI/NDK wiring for SQLite3/libcurl
  - The `fastfetch`-backed `/specs` (see main `wiki-chat.cpp`) has no Android equivalent to shell out to;
    needs a native Android hardware-info source (`android.os.Build`, `ActivityManager.MemoryInfo`, etc.)
  - Platform self-detection: bare `uname()` reports "Linux" on Android same as a Linux desktop (Android
    runs the Linux kernel) -- needs something Android-specific (e.g. checking for `/system/build.prop`, or
    just hardcoding it since a dedicated Android build already knows what it is).
- The APK builds but hasn't been installed/run on a real device or emulator yet (no emulator available in
  the environment it was built in) -- next real step is side-loading the debug APK onto an actual arm64-v8a
  device (a Galaxy S26 Ultra is the intended first test device) to confirm it actually launches and chats.
