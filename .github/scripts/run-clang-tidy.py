#!/usr/bin/env python3
import concurrent.futures
import json
import os
import subprocess
import sys
from pathlib import Path


def read_jobs() -> int:
    for name in ("CLANG_TIDY_JOBS", "CMAKE_BUILD_PARALLEL_LEVEL"):
        value = os.environ.get(name)
        if not value:
            continue
        try:
            jobs = int(value)
        except ValueError:
            continue
        if jobs > 0:
            return jobs
    return max(1, min(os.cpu_count() or 1, 4))


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

    jobs = read_jobs()

    print(f"Running clang-tidy for {len(files)} project source files with {jobs} jobs")
    for path in files:
        print(Path(path).relative_to(root).as_posix())

    cmd = [
        "clang-tidy",
        "--quiet",
        "-p",
        str(build),
        "--extra-arg=-Wno-unknown-warning-option",
    ]

    def run_one(path: str):
        proc = subprocess.run(cmd + [path], text=True, capture_output=True, check=False)
        return path, proc.returncode, proc.stdout, proc.stderr

    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = [executor.submit(run_one, path) for path in files]
        for future in concurrent.futures.as_completed(futures):
            path, returncode, stdout, stderr = future.result()
            if returncode != 0:
                failures.append((path, returncode, stdout, stderr))

    if failures:
        for path, returncode, stdout, stderr in failures:
            rel = Path(path).relative_to(root).as_posix()
            print(f"\nclang-tidy failed for {rel} (exit {returncode})", file=sys.stderr)
            if stdout:
                print(stdout, file=sys.stderr, end="" if stdout.endswith("\n") else "\n")
            if stderr:
                print(stderr, file=sys.stderr, end="" if stderr.endswith("\n") else "\n")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
