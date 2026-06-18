#!/usr/bin/env python3
import argparse
import glob
import re
import sys
from pathlib import Path


MODULE_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
STRING_RE = re.compile(r'"((?:\\.|[^"\\])*)"')


def strip_comments(text: str) -> str:
    out = []
    i = 0
    state = "normal"
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "normal":
            if ch == "/" and nxt == "/":
                out.extend("  ")
                i += 2
                state = "line"
                continue
            if ch == "/" and nxt == "*":
                out.extend("  ")
                i += 2
                state = "block"
                continue
            if ch == '"':
                out.append(ch)
                i += 1
                state = "string"
                continue
            out.append(ch)
            i += 1
        elif state == "line":
            out.append("\n" if ch == "\n" else " ")
            state = "normal" if ch == "\n" else "line"
            i += 1
        elif state == "block":
            if ch == "*" and nxt == "/":
                out.extend("  ")
                i += 2
                state = "normal"
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
        elif state == "string":
            out.append(ch)
            if ch == "\\" and nxt:
                out.append(nxt)
                i += 2
            elif ch == '"':
                i += 1
                state = "normal"
            else:
                i += 1
    return "".join(out)


def line_col(text: str, offset: int) -> str:
    line = text.count("\n", 0, offset) + 1
    col = offset - text.rfind("\n", 0, offset)
    return f"{line}:{col}"


def check_balanced(path: Path, text: str) -> list[str]:
    pairs = {"{": "}", "[": "]", "(": ")"}
    closing = {value: key for key, value in pairs.items()}
    stack: list[tuple[str, int]] = []
    failures: list[str] = []
    i = 0
    in_string = False
    while i < len(text):
        ch = text[i]
        if in_string:
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                in_string = False
            i += 1
            continue
        if ch == '"':
            in_string = True
        elif ch in pairs:
            stack.append((ch, i))
        elif ch in closing:
            if not stack or stack[-1][0] != closing[ch]:
                failures.append(f"{path}:{line_col(text, i)} unmatched {ch!r}")
            else:
                stack.pop()
        i += 1
    for ch, offset in stack:
        failures.append(f"{path}:{line_col(text, offset)} unmatched {ch!r}")
    if in_string:
        failures.append(f"{path}: unterminated string literal")
    return failures


def find_matching(text: str, start: int, open_ch: str, close_ch: str) -> int:
    depth = 0
    i = start
    in_string = False
    while i < len(text):
        ch = text[i]
        if in_string:
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                in_string = False
            i += 1
            continue
        if ch == '"':
            in_string = True
        elif ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def iter_module_blocks(text: str):
    depth = 0
    i = 0
    in_string = False
    while i < len(text):
        ch = text[i]
        if in_string:
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                in_string = False
            i += 1
            continue
        if ch == '"':
            in_string = True
            i += 1
            continue
        if ch == "{":
            depth += 1
            i += 1
            continue
        if ch == "}":
            depth -= 1
            i += 1
            continue
        if depth == 0:
            match = MODULE_RE.match(text, i)
            if match:
                j = match.end()
                while j < len(text) and text[j].isspace():
                    j += 1
                if j < len(text) and text[j] == "{":
                    end = find_matching(text, j, "{", "}")
                    if end == -1:
                        return
                    yield match.group(0), text[j + 1 : end], i
                    i = end + 1
                    continue
            i += 1
        else:
            i += 1


def extract_strings_from_array(block: str, prop: str) -> list[str]:
    values: list[str] = []
    pattern = re.compile(rf"\b{re.escape(prop)}\s*:\s*\[")
    for match in pattern.finditer(block):
        open_index = block.find("[", match.start())
        close_index = find_matching(block, open_index, "[", "]")
        if close_index == -1:
            continue
        values.extend(m.group(1) for m in STRING_RE.finditer(block[open_index + 1 : close_index]))
    return values


def validate_path_list(bp_path: Path, prop: str, values: list[str]) -> list[str]:
    failures: list[str] = []
    base = bp_path.parent
    for value in values:
        if value.startswith(("//", ":", "$")) or "$" in value:
            continue
        candidate = base / value
        if any(ch in value for ch in "*?["):
            matches = glob.glob(str(candidate), recursive=True)
            if not matches:
                failures.append(f"{bp_path}: {prop} glob has no matches: {value}")
        elif prop == "srcs":
            if not candidate.is_file():
                failures.append(f"{bp_path}: srcs entry is missing: {value}")
        elif not candidate.is_dir():
            failures.append(f"{bp_path}: {prop} directory is missing: {value}")
    return failures


def check_file(path: Path, names: dict[str, Path]) -> list[str]:
    raw = path.read_text(encoding="utf-8")
    text = strip_comments(raw)
    failures = check_balanced(path, text)
    module_count = 0
    for module_type, block, offset in iter_module_blocks(text):
        module_count += 1
        name_match = re.search(r'\bname\s*:\s*"((?:\\.|[^"\\])*)"', block)
        if not name_match:
            failures.append(f"{path}:{line_col(text, offset)} {module_type} module is missing name")
            continue
        name = name_match.group(1)
        if name in names:
            failures.append(f"{path}: duplicate Android module name {name!r}; first seen in {names[name]}")
        else:
            names[name] = path
        for prop in ("srcs", "local_include_dirs", "export_include_dirs"):
            failures.extend(validate_path_list(path, prop, extract_strings_from_array(block, prop)))
    if module_count == 0:
        failures.append(f"{path}: no Android.bp module blocks found")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate Android.bp files without modifying them.")
    parser.add_argument("root", nargs="?", default=".")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    bp_files = sorted(path for path in root.rglob("Android.bp") if ".git" not in path.parts)
    if not bp_files:
        print("No Android.bp files found.", file=sys.stderr)
        return 1

    names: dict[str, Path] = {}
    failures: list[str] = []
    for path in bp_files:
        failures.extend(check_file(path, names))

    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print(f"Android.bp validation passed: {len(bp_files)} file(s), {len(names)} module(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
