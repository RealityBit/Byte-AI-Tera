# Byte AI Android port -- work in progress

Started on the `android-port` branch. Not yet functional; this is scaffolding + direction, not a build.

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
design reference for the Android UI -- not code to port directly (it's vanilla JS/HTML, the Android app
will be Kotlin/Compose). The look to carry forward: black background, terminal-green (`#00ff00`) accents,
monospace font -- matches Byte's CLI identity in `Byte-AI-Tera` itself.

## What still needs real work (not started)

- `app-scaffold/lib`'s JNI layer needs the model wired to Byte's actual GGUF (currently generic
  llama.android sample wiring).
- A Compose UI matching the terminal-green aesthetic above, replacing llama.android's default sample UI.
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
- No Android Studio/emulator is available in the environment this was scaffolded in, so none of this has
  been built or run yet -- next real step needs an environment that can actually compile and launch it.
