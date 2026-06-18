#!/usr/bin/env python3
import json
import os
from pathlib import Path


def main() -> None:
    root = Path.cwd().resolve()
    build = (root / os.environ["BUILD_DIR"]).resolve()
    cache_path = build / "CMakeCache.txt"
    compile_db_path = build / "compile_commands.json"

    cache = {}
    for line in cache_path.read_text(errors="replace").splitlines():
        if not line or line.startswith(("//", "#")) or "=" not in line:
            continue
        key_type, value = line.split("=", 1)
        key = key_type.split(":", 1)[0]
        cache[key] = value

    failures = []
    for raw in os.environ.get("REQUIRED_CACHE", "").splitlines():
        raw = raw.strip()
        if not raw:
            continue
        key, expected = raw.split("=", 1)
        actual = cache.get(key)
        if actual != expected:
            failures.append(f"CMake cache {key} expected {expected}, got {actual!r}")

    entries = json.loads(compile_db_path.read_text())
    source_files = []
    compile_commands = []
    for entry in entries:
        compile_commands.append(entry.get("command", ""))
        path = Path(entry["file"]).resolve()
        try:
            rel = path.relative_to(root).as_posix()
        except ValueError:
            continue
        source_files.append(rel)

    for raw in os.environ.get("REQUIRED_SOURCE_PREFIXES", "").splitlines():
        prefix = raw.strip()
        if not prefix:
            continue
        if not any(path.startswith(prefix) for path in source_files):
            failures.append(f"compile_commands.json is missing sources under {prefix}")

    joined_commands = "\n".join(compile_commands)
    for raw in os.environ.get("REQUIRED_DEFINES", "").splitlines():
        define = raw.strip()
        if not define:
            continue
        if f"-D{define}" not in joined_commands and f"/D{define}" not in joined_commands:
            failures.append(f"compile_commands.json is missing compile definition {define}")

    if failures:
        for failure in failures:
            print(failure)
        raise SystemExit(1)

    print("CMake CI feature assertions passed.")
    print(f"compile_commands.json project source entries: {len(source_files)}")


if __name__ == "__main__":
    main()
