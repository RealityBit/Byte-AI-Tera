#!/usr/bin/env python3
"""
Generates Byte AI's distilled knowledge base from a bigger local model (Qwen2.5
3B via Ollama), split into chunks small enough for GitHub, matching the
category manifest at data/knowledge/categories.json. Fetched dynamically at
runtime by the category_fetch module (see modules/category_fetch.cpp).

Usage:
    ./generate_knowledge.py [category ...]   # omit to generate every category
"""
import json
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = ROOT / "data" / "knowledge" / "categories.json"
OLLAMA_URL = "http://localhost:11434/api/generate"
MODEL = "qwen2.5:3b"
FACTS_PER_CATEGORY = 20
MAX_CHUNK_BYTES = 4096  # well under GitHub's 100MB cap; keeps fetches fast

PROMPT_TEMPLATE = (
    "List {n} concise, accurate, standalone facts about {category}. "
    "One fact per line, no numbering, no headers, no extra commentary -- "
    "just the facts, each a single short sentence."
)


def ollama_generate(prompt: str) -> str:
    body = json.dumps({"model": MODEL, "prompt": prompt, "stream": False}).encode()
    req = urllib.request.Request(OLLAMA_URL, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        return json.loads(resp.read())["response"]


def chunk_facts(facts: list[str]) -> list[str]:
    chunks, current = [], ""
    for fact in facts:
        line = fact + "\n"
        if current and len(current) + len(line) > MAX_CHUNK_BYTES:
            chunks.append(current)
            current = ""
        current += line
    if current:
        chunks.append(current)
    return chunks


def generate_category(name: str) -> None:
    print(f"generating: {name}")
    prompt = PROMPT_TEMPLATE.format(n=FACTS_PER_CATEGORY, category=name)
    response = ollama_generate(prompt)

    facts = [line.strip("-* \t") for line in response.splitlines() if line.strip()]
    chunks = chunk_facts(facts)

    out_dir = ROOT / "data" / "knowledge" / name
    out_dir.mkdir(parents=True, exist_ok=True)
    for i, chunk in enumerate(chunks, start=1):
        path = out_dir / f"chunk-{i}.txt"
        path.write_text(chunk)
        print(f"  wrote {path.relative_to(ROOT)} ({len(chunk)} bytes)")


def main() -> None:
    manifest = json.loads(MANIFEST_PATH.read_text())
    all_names = [c["name"] for c in manifest["categories"]]

    requested = sys.argv[1:] or all_names
    unknown = set(requested) - set(all_names)
    if unknown:
        print(f"unknown categories: {', '.join(unknown)}", file=sys.stderr)
        print(f"available: {', '.join(all_names)}", file=sys.stderr)
        sys.exit(1)

    for name in requested:
        generate_category(name)


if __name__ == "__main__":
    main()
