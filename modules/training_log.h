#pragma once

#include <string>

// appends a fetched fact (Wikipedia summary, news snapshot, ...) to a local
// plain-text training corpus, so it can later be folded back into the model
// with llama-finetune (see retrain.sh). skips exact duplicates already
// present in the file, so re-asking about a cached topic doesn't spam it.
void training_log_append(const std::string & topic, const std::string & content, const std::string & log_path);
