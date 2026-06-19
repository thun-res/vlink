#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path

import yaml


def iter_uses(value):
    if isinstance(value, dict):
        for key, item in value.items():
            if key == "uses" and isinstance(item, str):
                yield item
            yield from iter_uses(item)
    elif isinstance(value, list):
        for item in value:
            yield from iter_uses(item)


def check_local(root: Path, uses: str) -> str | None:
    target = (root / uses[2:]).resolve()
    try:
        target.relative_to(root)
    except ValueError:
        return f"Local uses path escapes repository: {uses}"
    if target.is_file():
        return None
    if target.is_dir() and ((target / "action.yml").is_file() or (target / "action.yaml").is_file() or (target / "Dockerfile").is_file()):
        return None
    return f"Local uses path does not resolve to an action/workflow: {uses}"


def check_remote_tag(cache: dict[tuple[str, str], bool], uses: str) -> str | None:
    if uses.startswith("docker://"):
        return None
    if "@" not in uses:
        return f"Remote uses ref is missing: {uses}"
    spec, ref = uses.rsplit("@", 1)
    parts = spec.split("/")
    if len(parts) < 2:
        return f"Remote uses repo is invalid: {uses}"
    repo = "/".join(parts[:2])
    key = (repo, ref)
    if key not in cache:
        url = f"https://github.com/{repo}.git"
        result = subprocess.run(
            ["git", "ls-remote", "--exit-code", "--refs", "--tags", url, f"refs/tags/{ref}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        cache[key] = result.returncode == 0
    if not cache[key]:
        return f"Remote action tag does not exist: {uses}"
    return None


def main() -> int:
    root = Path.cwd().resolve()
    failures: list[str] = []
    cache: dict[tuple[str, str], bool] = {}
    files = sorted((root / ".github").rglob("*.yml")) + sorted((root / ".github").rglob("*.yaml"))
    for path in files:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
        for uses in iter_uses(data):
            if uses.startswith("./"):
                failure = check_local(root, uses)
            else:
                failure = check_remote_tag(cache, uses)
            if failure:
                failures.append(f"{path.relative_to(root)}: {failure}")

    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
