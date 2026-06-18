#!/usr/bin/env python3
import json
import os
import subprocess
from pathlib import Path


def main() -> None:
    root = Path.cwd().resolve()
    build = (root / os.environ["BUILD_DIR"]).resolve()
    entries = json.loads((build / "compile_commands.json").read_text())
    skip_dirs = {
        "thirdparty",
        "builtin",
        "prebuilt",
        "build-tidy",
        "build-conan",
        "build-deb",
        "build-rpm",
        "build-arch",
    }
    skip_prefixes = ("tools/android-bp/",)

    files = []
    for entry in entries:
        path = Path(entry["file"]).resolve()
        if path.suffix not in {".cc", ".cpp", ".cxx"}:
            continue
        try:
            rel_path = path.relative_to(root)
        except ValueError:
            continue
        rel = rel_path.as_posix()
        if any(part in skip_dirs for part in rel_path.parts):
            continue
        if any(rel.startswith(prefix) for prefix in skip_prefixes):
            continue
        files.append(str(path))

    files = sorted(set(files))
    if not files:
        raise SystemExit("No project source files found for clang-tidy")

    print(f"Running clang-tidy for {len(files)} project source files")
    for path in files:
        print(Path(path).relative_to(root).as_posix())

    cmd = ["clang-tidy", "--quiet", "-p", str(build)]
    for path in files:
        subprocess.run(cmd + [path], check=True)


if __name__ == "__main__":
    main()
