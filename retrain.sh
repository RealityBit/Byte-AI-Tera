#!/usr/bin/env bash
# Folds wiki-chat-training.txt (facts fetched from Wikipedia/news during chat)
# back into the model, producing a new gguf to load next time you launch
# llama-wiki-chat -- i.e. the model "updates on reboot".
#
# Finetuning in llama.cpp is WIP: it only works on an FP32 model and is slow
# on CPU. See examples/training/README.md for the upstream proof of concept
# this is based on.
#
# Usage:
#   ./retrain.sh <base-fp32-model.gguf> [training-log.txt] [output.gguf]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_BIN="$SCRIPT_DIR/../../build/bin/llama-finetune"

BASE_MODEL="${1:?usage: retrain.sh <base-fp32-model.gguf> [training-log.txt] [output.gguf]}"
TRAIN_LOG="${2:-wiki-chat-training.txt}"
OUT_MODEL="${3:-wiki-chat-finetuned.gguf}"

if [ ! -f "$TRAIN_LOG" ]; then
    echo "no training log found at $TRAIN_LOG yet -- chat a bit first so it has facts to learn from" >&2
    exit 1
fi

"$BUILD_BIN" \
    --file "$TRAIN_LOG" \
    --model "$BASE_MODEL" \
    -ngl 999 -c 512 -b 512 -ub 512 \
    -o "$OUT_MODEL"

echo "done: point -m at $OUT_MODEL next time you launch llama-wiki-chat"
