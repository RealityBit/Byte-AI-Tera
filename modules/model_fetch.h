#pragma once

#include <string>

// downloads and reassembles a chunked GGUF model file from a manifest hosted
// on GitHub (see https://github.com/RetroGigabyte/Byte-AI-Models), verifying
// the final file's sha256 against the manifest. Safe to re-run after an
// interruption -- it resumes from the last fully-downloaded chunk rather
// than starting over. Prints progress to stdout as it goes.
//
// returns true if output_path now holds a complete, checksum-verified model.
bool model_fetch(const std::string & manifest_url, const std::string & output_path);
